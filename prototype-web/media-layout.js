export function eyeRegion(layout, eye, swap = false) {
  const index = swap ? 1 - eye : eye;
  if (layout === 'sbs') return [0.5, 1, index * 0.5, 0];
  if (layout === 'tb') return [1, 0.5, 0, (1 - index) * 0.5];
  return [1, 1, 0, 0];
}

export function displayAspect(width, height, layout) {
  if (!width || !height) return 16 / 9;
  const [x, y] = eyeRegion(layout, 0);
  return (width * x) / (height * y);
}

export function formatTime(seconds) {
  const time = Math.floor(Number.isFinite(seconds) ? Math.max(0, seconds) : 0);
  const hours = Math.floor(time / 3600);
  const minutes = Math.floor(time / 60) % 60;
  const remainder = String(time % 60).padStart(2, '0');
  return hours ? `${hours}:${String(minutes).padStart(2, '0')}:${remainder}` : `${minutes}:${remainder}`;
}
