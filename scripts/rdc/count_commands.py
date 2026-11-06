# Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
# qrenderdoc --python count_commands.py   (env RDC, OUT): counts Vulkan commands in a captured
# frame (render passes, dispatches, copies, draws), to compare GPU work between platforms.
import os, sys, collections
import renderdoc as rd
out = open(os.environ["OUT"], "w")
cap = rd.OpenCaptureFile()
if cap.OpenFile(os.environ["RDC"], "", None) != rd.ResultCode.Succeeded: out.write("open failed\n"); os._exit(1)
res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
if res != rd.ResultCode.Succeeded: out.write("replay failed\n"); os._exit(1)
sf = ctrl.GetStructuredFile()
counts = collections.Counter()
def walk(actions):
    for a in actions:
        name = a.GetName(sf)
        for key in ("vkCmdBeginRenderPass", "vkCmdBeginRendering", "vkCmdDispatch", "vkCmdCopyBufferToImage",
                    "vkCmdCopyBuffer(", "vkCmdCopyImage", "vkCmdBlitImage", "vkCmdDraw", "vkCmdPipelineBarrier",
                    "vkQueueSubmit", "vkCmdClear"):
            if name.startswith(key.rstrip("(")) and (key != "vkCmdCopyBuffer(" or name.startswith("vkCmdCopyBuffer(")):
                counts[key.rstrip("(")] += 1
        walk(a.children)
walk(ctrl.GetRootActions())
for k, v in sorted(counts.items()): out.write(f"{k} {v}\n")
ctrl.Shutdown(); cap.Shutdown(); out.close()
os._exit(0)  # qrenderdoc keeps its UI open otherwise
