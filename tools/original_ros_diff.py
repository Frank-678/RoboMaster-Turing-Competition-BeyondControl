#!/usr/bin/env python3
"""original_ros_diff —— 原版 ROS 席位差分适配器（review.md 第 4 条 / docs/49 配套）。

接口面（bot.cpp:43-58）：/team_<seat>/perception（订阅，公开感知 JSON）、
/team_<seat>/action（发布，动作 JSON）、/team_<seat>/registration（transient_local 保留）。
域 ID = match_config.hpp ros_domain_id（默认 27）。

模式：
  observe（默认）——只订阅 perception/action，录制 (tick, perception, action) 磁带到 --out。
      公开话题、只读观测；不读排行榜/管理端，不向参赛程序注入任何真值。
  drive —— 先发 registration，再逐 tick 按 --policy（echo|tape:<path>）发布动作，
      用作脚本席位；用于与自建引擎（tests/run_scenarios.py）逐 Tick 差分。

用法：
  python3 tools/original_ros_diff.py --seat B observe --out /tmp/tape_B.jsonl
  python3 tools/original_ros_diff.py --seat B drive --policy tape:/tmp/tape_A.jsonl
"""
import argparse
import json
import os
import sys
import time

import rclpy
from rclpy.node import Node
from std_msgs.msg import String


class DiffAdapter(Node):
    def __init__(self, seat, mode, policy, out):
        prefix = "/team_" + seat
        super().__init__("original_ros_diff_" + seat)
        self.tape = {}
        self.order = []
        self.out = out
        self.mode = mode
        self.policy = policy
        self.replay = {}
        if mode == "drive" and policy.startswith("tape:"):
            for line in open(policy.split(":", 1)[1], encoding="utf-8"):
                rec = json.loads(line)
                self.replay[int(rec["tick"])] = rec["action"]
        self.action_pub = None
        self.reg_pub = None
        if mode == "drive":
            qos_retained = rclpy.qos.QoSProfile(
                depth=1, reliability=rclpy.qos.ReliabilityPolicy.RELIABLE,
                durability=rclpy.qos.DurabilityPolicy.TRANSIENT_LOCAL)
            self.reg_pub = self.create_publisher(String, prefix + "/registration", qos_retained)
            self.action_pub = self.create_publisher(String, prefix + "/action", 1)
            self.reg_pub.publish(String(data=json.dumps(
                {"team": seat, "name": "original_ros_diff"})))
            self.get_logger().info("drive seat=%s registered" % seat)
        self.create_subscription(
            String, prefix + "/perception",
            lambda m: self.on_perception(m), 1)
        self.create_subscription(
            String, prefix + "/action",
            lambda m: self.on_action(m), 1)

    def on_perception(self, msg):
        if self.mode != "observe":
            try:
                p = json.loads(msg.data)
                tick = int(p["tick"])
                act = self.replay.get(tick)
                if act is None:
                    self.get_logger().warn("no tape action for tick %d" % tick)
                    return
                self.action_pub.publish(String(data=json.dumps(act)))
            except Exception as e:
                self.get_logger().error("drive error: %s" % e)
            return
        try:
            p = json.loads(msg.data)
            tick = int(p["tick"])
        except Exception:
            return
        rec = self.tape.setdefault(tick, {"perception": json.loads(msg.data)})
        rec["tick"] = tick
        self.flush_if_done(tick)

    def on_action(self, msg):
        if self.mode != "observe":
            return
        try:
            act = json.loads(msg.data)
        except Exception:
            return
        # action 无 tick 字段；归属最近一个已收到感知的 tick。
        if not self.order:
            return
        tick = self.order[-1]
        rec = self.tape.setdefault(tick, {})
        rec["tick"] = tick
        rec["action"] = act
        self.flush_if_done(tick)

    def flush_if_done(self, tick):
        rec = self.tape.get(tick)
        if rec and "perception" in rec and "action" in rec:
            if tick not in self.order:
                self.order.append(tick)
            # 按序冲刷：把 <= 当前 tick 的完整记录写出。
            while self.order:
                t = self.order[0]
                r = self.tape.get(t)
                if r and "perception" in r and "action" in r:
                    self.out.write(json.dumps(r, ensure_ascii=False) + "\n")
                    self.out.flush()
                    self.order.pop(0)
                    del self.tape[t]
                else:
                    break


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seat", required=True, choices=list("ABCD"))
    sub = ap.add_subparsers(dest="mode", required=True)
    ob = sub.add_parser("observe")
    ob.add_argument("--out", default="/tmp/original_ros_diff_tape.jsonl")
    dr = sub.add_parser("drive")
    dr.add_argument("--policy", required=True, help="echo|tape:<path>")
    args = ap.parse_args()
    os.environ.setdefault("ROS_DOMAIN_ID", "27")
    rclpy.init()
    out = None
    if args.mode == "observe":
        out = open(args.out, "w", encoding="utf-8")
    node = DiffAdapter(args.seat, args.mode,
                       getattr(args, "policy", "echo"), out)
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        if out:
            out.close()
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
