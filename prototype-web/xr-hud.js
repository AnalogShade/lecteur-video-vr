import {
  CanvasTexture, Group, Line, LineBasicMaterial, BufferGeometry, Float32BufferAttribute,
  Mesh, MeshBasicMaterial, PlaneGeometry, Raycaster, SRGBColorSpace, Vector3,
} from 'three';
import { formatTime } from './media-layout.js';

const width = 1024;
const height = 304;
const buttons = [
  { x: 35, width: 105, action: 'back' },
  { x: 153, width: 173, action: 'play' },
  { x: 339, width: 105, action: 'forward' },
  { x: 457, width: 158, action: 'mute' },
  { x: 628, width: 193, action: 'reset' },
  { x: 834, width: 155, action: 'next' },
];

function buttonAt(x, y) {
  if (x >= 35 && x <= 989 && y >= 56 && y <= 131) return { action: 'seek', fraction: (x - 35) / 954 };
  if (y >= 143 && y <= 221) return buttons.find(button => x >= button.x && x <= button.x + button.width);
  if (y >= 234 && y <= 294) {
    if (x >= 35 && x <= 305) return { action: 'projection' };
    if (x >= 318 && x <= 672) return { action: 'layout' };
    if (x >= 685 && x <= 808) return { action: 'hide' };
    if (x >= 821 && x <= 989) return { action: 'exit' };
  }
  return null;
}

export function createHud(scene, renderer, getState, onAction) {
  const canvas = document.createElement('canvas');
  canvas.width = width;
  canvas.height = height;
  const context = canvas.getContext('2d');
  const texture = new CanvasTexture(canvas);
  texture.colorSpace = SRGBColorSpace;
  const group = new Group();
  const panel = new Mesh(
    new PlaneGeometry(1.52, 1.52 * height / width),
    new MeshBasicMaterial({ map: texture, transparent: true, depthTest: false, depthWrite: false }),
  );
  panel.renderOrder = 2;
  group.add(panel);
  group.visible = false;
  scene.add(group);

  const origin = new Vector3();
  const direction = new Vector3();
  const ray = new Raycaster();
  let hover = '';
  let lastDraw = '';
  const controllers = [0, 1].map(index => {
    const controller = renderer.xr.getController(index);
    const geometry = new BufferGeometry();
    geometry.setAttribute('position', new Float32BufferAttribute([0, 0, 0, 0, 0, -1], 3));
    const line = new Line(geometry, new LineBasicMaterial({ color: 0xc8b9ff, transparent: true, opacity: 0.8, depthTest: false }));
    line.renderOrder = 3;
    controller.add(line);
    controller.userData.connected = false;
    controller.addEventListener('connected', () => { controller.userData.connected = true; });
    controller.addEventListener('disconnected', () => { controller.userData.connected = false; });
    controller.addEventListener('select', () => {
      if (!group.visible) return onAction('show');
      const hit = aim(controller);
      const button = hit && buttonAt(hit.uv.x * width, (1 - hit.uv.y) * height);
      if (button) onAction(button.action, button.fraction);
    });
    controller.addEventListener('squeeze', () => onAction(group.visible ? 'hide' : 'show'));
    scene.add(controller);
    return { controller, line };
  });

  function aim(controller) {
    origin.setFromMatrixPosition(controller.matrixWorld);
    direction.set(0, 0, -1).transformDirection(controller.matrixWorld);
    ray.set(origin, direction);
    return ray.intersectObject(panel, false)[0];
  }

  function rounded(x, y, w, h, radius, color) {
    context.fillStyle = color;
    context.beginPath();
    context.roundRect(x, y, w, h, radius);
    context.fill();
  }

  function label(value, x, y, size = 29, color = '#f1edff', align = 'left', maxWidth = width) {
    context.font = `600 ${size}px system-ui, sans-serif`;
    context.textBaseline = 'middle';
    context.textAlign = align;
    context.fillStyle = color;
    context.fillText(value, x, y, maxWidth);
  }

  function draw() {
    const state = getState();
    const elapsed = formatTime(state.time);
    const duration = formatTime(state.duration);
    const progress = state.duration > 0 ? Math.round(state.time * 1000 / state.duration) / 1000 : 0;
    const signature = JSON.stringify([state.name, elapsed, duration, progress, state.paused, state.muted, state.projection, state.layout, state.next, hover]);
    if (signature === lastDraw) return;
    lastDraw = signature;
    context.clearRect(0, 0, width, height);
    rounded(0, 0, width, height, 29, '#141525');
    label(state.name, 35, 31, 30, '#f5f2ff', 'left', 950);
    label(`${elapsed} / ${duration}`, 989, 76, 23, '#d9cdfa', 'right');
    rounded(35, 105, 954, 13, 6, hover === 'seek' ? '#655889' : '#393750');
    if (progress > 0) rounded(35, 105, 954 * progress, 13, 6, '#baabff');

    const labels = ['−10 s', state.paused ? 'Lire' : 'Pause', '+10 s', state.muted ? 'Son coupé' : 'Son actif', 'Recentrer', 'Suivante'];
    buttons.forEach(({ x, width: buttonWidth, action }, index) => {
      const active = hover === action;
      const primary = action === 'play';
      const disabled = action === 'next' && !state.next;
      rounded(x, 143, buttonWidth, 78, 15, primary ? (active ? '#d3c9ff' : '#baabff') : (active ? '#4e446e' : '#282840'));
      label(labels[index], x + buttonWidth / 2, 183, 27, primary ? '#211741' : (disabled ? '#8e88a0' : '#f1edff'), 'center');
    });

    const projection = { flat: 'Écran plat', 180: 'VR 180°', 360: 'VR 360°' }[state.projection];
    const layout = { mono: 'Mono', sbs: '3D côte à côte', tb: '3D haut / bas' }[state.layout];
    const options = [
      ['projection', 35, 270, projection], ['layout', 318, 354, layout],
      ['hide', 685, 123, 'Masquer'], ['exit', 821, 168, 'Quitter la VR'],
    ];
    for (const [action, x, w, value] of options) {
      rounded(x, 234, w, 54, 12, hover === action ? '#4e446e' : '#232338');
      label(value, x + w / 2, 261, 24, '#e5dcff', 'center');
    }
    texture.needsUpdate = true;
  }

  return {
    group,
    update() {
      hover = '';
      for (const { controller, line } of controllers) {
        line.visible = group.visible && controller.userData.connected && controller.visible;
        if (!line.visible) continue;
        const hit = aim(controller);
        line.scale.z = hit?.distance ?? 2;
        if (hit) hover = buttonAt(hit.uv.x * width, (1 - hit.uv.y) * height)?.action ?? hover;
      }
      if (group.visible) draw();
    },
  };
}
