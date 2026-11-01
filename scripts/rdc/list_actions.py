# qrenderdoc --python list_actions.py   (env RDC, OUT): summary of actions in a capture.
import os, sys, collections
import renderdoc as rd
out = open(os.environ["OUT"], "w")
def log(*a): out.write(" ".join(str(x) for x in a) + "\n"); out.flush()
cap = rd.OpenCaptureFile()
if cap.OpenFile(os.environ["RDC"], "", None) != rd.ResultCode.Succeeded: log("open failed"); sys.exit(1)
res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
if res != rd.ResultCode.Succeeded: log("replay failed", res); sys.exit(1)
sf = ctrl.GetStructuredFile()
kinds = collections.Counter()
def walk(actions, depth=0):
    for a in actions:
        kinds[str(a.flags)] += 1
        if depth < 3 and len(kinds) < 400:
            log("  " * depth + "ev", a.eventId, a.GetName(sf)[:100], "children", len(a.children))
        walk(a.children, depth + 1)
walk(ctrl.GetRootActions())
for k, v in kinds.most_common(): log("flags", k, v)
ctrl.Shutdown(); cap.Shutdown(); out.close()
os._exit(0)  # qrenderdoc keeps its UI open otherwise
