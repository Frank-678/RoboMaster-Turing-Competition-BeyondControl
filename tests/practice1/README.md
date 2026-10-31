# Stage 1 size-aligned test

The sample uses a 20x20 map, 100 ticks, one robot, and 16 resources worth 200. It is synthetic, not the official stage-one layout.

Run with `bash practice1/run.sh`. The runner checks geometry and invokes the unmodified practice validator before starting the original practice package. If validation fails, retain the error and do not claim compatibility.

The contestant must be built for stage 1. Do not run concurrently with another referee in the same DDS domain. This directory contains test configuration only.
