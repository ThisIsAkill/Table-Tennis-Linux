# Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
# qrenderdoc --python gpu_durations.py   (env RDC, OUT): GPU time per action type and the
# most expensive actions in a capture (GPUDuration counter), with each draw's target size.
import os, sys, collections
import renderdoc as rd
out = open(os.environ["OUT"], "w")
def log(*a): out.write(" ".join(str(x) for x in a) + "\n"); out.flush()
cap = rd.OpenCaptureFile()
if cap.OpenFile(os.environ["RDC"], "", None) != rd.ResultCode.Succeeded: log("open failed"); sys.exit(1)
res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
if res != rd.ResultCode.Succeeded: log("replay failed", res); sys.exit(1)
sf = ctrl.GetStructuredFile()
names = {}
def walk(actions):
    for a in actions:
        names[a.eventId] = a
        walk(a.children)
walk(ctrl.GetRootActions())
results = ctrl.FetchCounters([rd.GPUCounter.EventGPUDuration])
desc = ctrl.DescribeCounter(rd.GPUCounter.EventGPUDuration)
dur = {}
for r in results:
    v = r.value.d if desc.resultByteWidth == 8 else r.value.f
    dur[r.eventId] = v * 1000.0  # ms
total = sum(dur.values())
log("events with duration", len(dur), "total GPU ms", round(total, 3))
by_kind = collections.defaultdict(lambda: [0.0, 0])
for e, ms in dur.items():
    a = names.get(e)
    kind = a.GetName(sf).split("(")[0] if a else "?"
    by_kind[kind][0] += ms; by_kind[kind][1] += 1
for k, (ms, n) in sorted(by_kind.items(), key=lambda x: -x[1][0])[:15]:
    log("kind %-40s %8.3f ms %5d events" % (k, ms, n))
# Target sizes of the 40 most expensive actions.
by_size = collections.defaultdict(lambda: [0.0, 0])
top = sorted(dur.items(), key=lambda x: -x[1])
for e, ms in top[:40]:
    ctrl.SetFrameEvent(e, False)
    st = ctrl.GetPipelineState()
    size = ""
    try:
        outs = st.GetOutputTargets()
        for o in outs:
            if o.resource != rd.ResourceId.Null():
                for t in ctrl.GetTextures():
                    if t.resourceId == o.resource:
                        size = "%dx%d" % (t.width, t.height); break
                break
    except Exception as ex:
        size = "?"
    log("ev %6d %7.3f ms %-12s %s" % (e, ms, size, names[e].GetName(sf)[:90]))
ctrl.Shutdown(); cap.Shutdown(); out.close()
os._exit(0)
