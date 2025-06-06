import { runRefTest } from './gpu_ref_test.js';

runRefTest(t => {
  function draw(canvasId: string, format: GPUTextureFormat) {
    const canvas = document.getElementById(canvasId) as HTMLCanvasElement;

    const ctx = canvas.getContext('webgpu') as unknown as GPUCanvasContext;
    ctx.configure({
      device: t.device,
      format,
    });

    const colorAttachment = ctx.getCurrentTexture();
    const colorAttachmentView = colorAttachment.createView();

    const encoder = t.device.createCommandEncoder({ label: 'runRefTest' });
    const pass = encoder.beginRenderPass({
      colorAttachments: [
        {
          view: colorAttachmentView,
          clearValue: { r: 0.4, g: 1.0, b: 0.0, a: 1.0 },
          loadOp: 'clear',
          storeOp: 'store',
        },
      ],
    });
    pass.end();
    t.device.queue.submit([encoder.finish()]);
  }

  draw('cvs0', 'bgra8unorm');
  draw('cvs1', 'rgba8unorm');
  draw('cvs2', 'rgba16float');
});
