// Plan B Sample Script for QuickJS External Generator Plugin
// Exports renderFrame(canvas, frame, fps, params)
// Canvas API provided by vse-quickjs-gen:
//   canvas.width, canvas.height
//   canvas.clear(r, g, b, a)
//   canvas.fillRect(x, y, w, h, r, g, b, a)

globalThis.renderFrame = function renderFrame(canvas, frame, fps, params) {
  const t = frame / Math.max(1, fps);
  const progress = Math.min(1, t / 1.5);
  canvas.clear(0, 0, 0, 0);

  const barW = Math.floor(canvas.width * 0.7 * progress);
  const barH = Math.max(12, Math.floor(canvas.height * 0.08));
  const x = Math.floor(canvas.width * 0.15);
  const y = Math.floor(canvas.height * 0.72);

  // Dark backing pill
  canvas.fillRect(x - 16, y - 16, Math.floor(canvas.width * 0.7) + 32, barH + 32, 18, 24, 38, 220);
  // Animated accent bar
  canvas.fillRect(x, y, barW, barH, 76, 201, 240, 255);
};
