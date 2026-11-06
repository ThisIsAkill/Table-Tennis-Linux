# Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
# qrenderdoc --python find_floor.py   (env RDC=capture path, OUT=result file)
# Lists draws whose fragment shader matches the floor shader's interface.
import os, sys
import renderdoc as rd

out = open(os.environ["OUT"], "w")
def log(*a):
    out.write(" ".join(str(x) for x in a) + "\n"); out.flush()

cap = rd.OpenCaptureFile()
res = cap.OpenFile(os.environ["RDC"], "", None)
if res != rd.ResultCode.Succeeded:
    log("open failed", res); out.close(); sys.exit(1)
res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
if res != rd.ResultCode.Succeeded:
    log("replay failed", res); out.close(); sys.exit(1)

def walk(actions):
    for a in actions:
        yield a
        yield from walk(a.children)

count = 0
seen = {}
for a in walk(ctrl.GetRootActions()):
    if not (a.flags & rd.ActionFlags.Drawcall):
        continue
    count += 1
    ctrl.SetFrameEvent(a.eventId, False)
    st = ctrl.GetPipelineState()
    refl = st.GetShaderReflection(rd.ShaderStage.Fragment)
    if refl is None:
        continue
    names = [r.name for r in refl.readOnlyResources]
    ins = [s.varName for s in refl.inputSignature]
    key = (len(ins), tuple(names))
    if key not in seen:
        seen[key] = a.eventId
        log("PS iface ev", a.eventId, "inputs", len(ins), ins[:8], "res", names)
    if any("texture5_cube" in n for n in names) and any("texture6_2d" in n for n in names) \
            and len([i for i in ins if "interpolator" in i]) == 7:
        outs = st.GetOutputTargets()
        log("FLOOR? event", a.eventId, "indices", a.numIndices, "inputs", ins,
            "targets", [(str(o.resource), ) for o in outs if o.resource != rd.ResourceId.Null()])
log("draws", count)
ctrl.Shutdown(); cap.Shutdown(); out.close()
os._exit(0)  # qrenderdoc keeps its UI open otherwise
