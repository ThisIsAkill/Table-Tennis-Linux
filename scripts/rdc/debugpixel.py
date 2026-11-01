# qrenderdoc --python debugpixel.py (env RDC, OUT, EVENT, X, Y): pixel shader inputs/outputs at a pixel.
import os, sys
import renderdoc as rd
out = open(os.environ["OUT"], "w")
def log(*a): out.write(" ".join(str(x) for x in a) + "\n"); out.flush()
cap = rd.OpenCaptureFile()
cap.OpenFile(os.environ["RDC"], "", None)
res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
ev = int(os.environ["EVENT"])
ctrl.SetFrameEvent(ev, True)
st = ctrl.GetPipelineState()
outs = st.GetOutputTargets()
tex = outs[0].resource
for t in ctrl.GetTextures():
    if t.resourceId == tex:
        log("target", tex, t.width, "x", t.height, "samples", t.msSamp, "format", t.format.Name())
vp = st.GetViewport(0); sc = st.GetScissor(0)
log("viewport", vp.x, vp.y, vp.width, vp.height, "scissor", sc.x, sc.y, sc.width, sc.height)
# Pixel history at a few points tells which pixels this draw touched.
pts = []
for gy in range(4, sc.height, 24):
    for gx in range(8, sc.width, 48):
        hist = ctrl.PixelHistory(tex, gx, gy, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        if any(h.eventId == ev for h in hist):
            pts.append((gx, gy))
    if len(pts) >= 2:
        break
log("pixels covered by this draw:", pts)
pts = pts[:1]
for (x, y) in pts:
    hist = ctrl.PixelHistory(tex, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
    log("history at", x, y, "events", sorted(set(h.eventId for h in hist))[:40])
    for h in hist:
        if h.eventId == ev:
            log("history ev", h.eventId, "passed", h.Passed(), "pre", list(h.preMod.col.floatValue),
                "shader_out", list(h.shaderOut.col.floatValue), "post", list(h.postMod.col.floatValue))
    inputs = rd.DebugPixelInputs()
    inputs.sample = 0
    trace = ctrl.DebugPixel(x, y, inputs)
    if trace.debugger is None:
        log("no debug trace at", x, y, "(sample 0)")
        continue
    states = ctrl.ContinueDebug(trace.debugger)
    if states:
        for c in states[0].changes:
            log("initial", c.after.name, [round(f, 6) for f in c.after.value.f32v[:4]])
    total = 0
    last = None
    trace_out = open(os.environ["OUT"] + ".trace", "w")
    def fmtv(v):
        if v.members:
            return "{" + ", ".join(m.name + "=" + fmtv(m) for m in v.members[:8]) + "}"
        n = max(v.columns, 1) * max(v.rows, 1)
        return str([round(f, 6) for f in v.value.f32v[:min(n, 4)]])
    if states:
        for c in states[0].changes:
            trace_out.write("init %s = %s\n" % (c.after.name, fmtv(c.after)))
    while states:
        for stt in states:
            for c in stt.changes:
                trace_out.write("%d line%s %s = %s\n" % (stt.stepIndex, stt.nextInstruction, c.after.name, fmtv(c.after)))
        total += len(states)
        last = states[-1]
        states = ctrl.ContinueDebug(trace.debugger)
    trace_out.close()
    log("steps", total)
    if last is not None:
        for c in last.changes[-12:]:
            log("last change", c.after.name, [round(f, 5) for f in c.after.value.f32v[:4]])
    ctrl.FreeDebugger(trace.debugger)
ctrl.Shutdown(); cap.Shutdown(); out.close()
os._exit(0)
