# qrenderdoc --python constants.py (env RDC, OUT, EVENT): fragment shader constant buffers of a draw.
import os, sys
import renderdoc as rd
out = open(os.environ["OUT"], "w")
def log(*a): out.write(" ".join(str(x) for x in a) + "\n"); out.flush()
cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RDC"], "", None)
res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
ev = int(os.environ["EVENT"]); ctrl.SetFrameEvent(ev, True)
st = ctrl.GetPipelineState()
stage = rd.ShaderStage.Fragment
refl = st.GetShaderReflection(stage)
def dump(v, prefix=""):
    if v.members:
        for m in v.members: dump(m, prefix + v.name + ".")
    else:
        vals = [round(x, 6) for x in v.value.f32v[:v.columns * max(v.rows, 1)]]
        u = list(v.value.u32v[:v.columns * max(v.rows, 1)])
        log(prefix + v.name, vals, [hex(x) for x in u])
for i, cb in enumerate(refl.constantBlocks):
    if cb.name not in ("xe_uniform_system_constants",):
        continue
    bind = st.GetConstantBlock(stage, i, 0)
    vars_ = ctrl.GetCBufferVariableContents(st.GetGraphicsPipelineObject(), refl.resourceId, stage,
                                            st.GetShaderEntryPoint(stage), i, bind.descriptor.resource,
                                            bind.descriptor.byteOffset, bind.descriptor.byteSize)
    log("== block", cb.name)
    for v in vars_: dump(v)
ctrl.Shutdown(); cap.Shutdown(); out.close()
os._exit(0)
