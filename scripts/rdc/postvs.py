# qrenderdoc --python postvs.py  (env RDC, OUT, EVENT): vertex shader outputs of one draw.
import os, sys, struct
import renderdoc as rd
out = open(os.environ["OUT"], "w")
def log(*a): out.write(" ".join(str(x) for x in a) + "\n"); out.flush()
cap = rd.OpenCaptureFile()
cap.OpenFile(os.environ["RDC"], "", None)
res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
ev = int(os.environ["EVENT"])
ctrl.SetFrameEvent(ev, True)
st = ctrl.GetPipelineState()
vs = st.GetShaderReflection(rd.ShaderStage.Vertex)
sig = [(s.varName, s.compCount, s.systemValue) for s in vs.outputSignature]
log("VS outputs:", sig)
m = ctrl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
log("postvs res", m.vertexResourceId, "off", m.vertexByteOffset, "stride", m.vertexByteStride,
    "numIndices", m.numIndices)
data = ctrl.GetBufferData(m.vertexResourceId, m.vertexByteOffset, m.vertexByteStride * max(m.numIndices, 4))
nfl = m.vertexByteStride // 4
for v in range(min(m.numIndices, 4)):
    vals = struct.unpack_from("%df" % nfl, data, v * m.vertexByteStride)
    log("vertex", v, " ".join("%.4g" % x for x in vals))
ctrl.Shutdown(); cap.Shutdown(); out.close()
os._exit(0)
