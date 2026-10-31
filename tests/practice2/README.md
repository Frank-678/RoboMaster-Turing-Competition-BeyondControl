# Stage 2 size-aligned test

The sample uses a 24x24 map, 500 ticks, four robots, and 16 resources worth 200. The map and event schedule are synthetic, not the official tournament map. Standard v1.2 event effect values are applied where the template supports them.

Run with `bash practice2/run.sh`. Mock AI is only for interface checks, not official qualification. The original practice validator runs before referee startup. If validation rejects the new map size, retain the error and do not modify original binaries.

Use the stage-2 contestant with matching network settings. Run sequentially with practice1 or isolate all DDS participants. Record configuration and binary hashes, score components, first resource timings, and any failed tests.

The 98-point ambition is a separate training objective across maximum Stage 1 40 and Stage 2 60. The tournament does not sum these scores to rank participants.
