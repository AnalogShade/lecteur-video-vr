import {
  createIcons, Film, FolderOpen, Glasses, Headset, ListVideo, Maximize, Move, Pause, Play, Plus,
  RotateCcw, RotateCw, ScanLine, ShieldCheck, SlidersHorizontal, Volume2, VolumeX, X,
} from 'lucide';
import { formatTime } from './media-layout.js';
import { createVideoScene } from './video-scene.js';

const icons = {
  Film, FolderOpen, Glasses, Headset, ListVideo, Maximize, Move, Pause, Play, Plus, RotateCcw,
  RotateCw, ScanLine, ShieldCheck, SlidersHorizontal, Volume2, VolumeX, X,
};
const $ = id => document.getElementById(id);
const video = $('media');
const files = $('files');
const screen = $('screen');
const playlist = [];
const projections = ['flat', '180', '360'];
const layouts = ['mono', 'sbs', 'tb'];
const defaultSettings = { projection: 'flat', layout: 'mono', swap: false };
const idleStatus = 'Les vidéos locales restent sur ton appareil.';
let current = null;
let xrSupported = false;
let session = null;
let entering = false;
let scene;

function status(message = idleStatus, error = false) {
  $('status').lastElementChild.textContent = message;
  $('status').classList.toggle('error', error);
}

function state() {
  return {
    ...(current?.settings ?? defaultSettings),
    active: Boolean(current),
    paused: video.paused,
    muted: video.muted || video.volume === 0,
    time: video.currentTime,
    duration: video.duration,
    name: current?.file.name ?? '',
    next: current !== null && playlist.indexOf(current) + 1 < playlist.length,
  };
}

function refreshSettings() {
  const options = state();
  for (const button of $('projection').querySelectorAll('button')) {
    button.setAttribute('aria-pressed', String(button.dataset.value === options.projection));
  }
  $('stereo').value = options.layout;
  $('swap').checked = options.swap;
  $('swap').disabled = options.layout === 'mono' || !scene;
  const orbit = Boolean(scene && current && options.projection !== 'flat');
  screen.classList.toggle('orbit', orbit);
  $('screen-hint').hidden = !orbit;
  scene?.update(options);
}

function refreshTransport() {
  const options = state();
  const playable = current !== null && !video.error;
  const seekable = playable && Number.isFinite(options.duration) && options.duration > 0;
  const seek = $('seek');
  seek.disabled = !seekable;
  seek.max = seekable ? options.duration : 1;
  seek.value = seekable ? options.time : 0;
  $('elapsed').textContent = formatTime(options.time);
  $('duration').textContent = formatTime(options.duration);
  $('play').disabled = !playable;
  $('back').disabled = !seekable;
  $('forward').disabled = !seekable;
  $('reset').disabled = !current || !scene || options.projection === 'flat';
  setToggle($('play'), !options.paused, 'Lire', 'Mettre en pause');
  setToggle($('mute'), options.muted, 'Couper le son', 'Rétablir le son');
  $('vr').disabled = entering || (!session && (!xrSupported || !playable));
  $('vr').lastElementChild.textContent = session ? 'Quitter la VR' : 'Entrer en VR';
}

function setToggle(button, enabled, offLabel, onLabel) {
  button.firstElementChild.hidden = enabled;
  button.lastElementChild.hidden = !enabled;
  const label = enabled ? onLabel : offLabel;
  button.setAttribute('aria-label', label);
  button.title = label;
}

function renderPlaylist() {
  $('count').textContent = playlist.length;
  $('queue-empty').hidden = playlist.length > 0;
  const rows = playlist.map(item => {
    const row = document.createElement('li');
    const select = document.createElement('button');
    select.className = 'queue-item';
    select.title = item.file.name;
    if (item === current) select.setAttribute('aria-current', 'true');
    const icon = document.createElement('i');
    icon.dataset.lucide = 'film';
    const name = document.createElement('span');
    name.textContent = item.file.name;
    select.append(icon, name);
    select.addEventListener('click', () => load(item));

    const remove = document.createElement('button');
    remove.className = 'icon-button';
    remove.title = 'Retirer de la file';
    remove.setAttribute('aria-label', `Retirer ${item.file.name} de la file`);
    const cross = document.createElement('i');
    cross.dataset.lucide = 'x';
    remove.append(cross);
    remove.addEventListener('click', () => {
      const index = playlist.indexOf(item);
      playlist.splice(index, 1);
      if (item === current) load(playlist[index] ?? playlist[index - 1] ?? null);
      else renderPlaylist();
      URL.revokeObjectURL(item.url);
    });
    row.append(select, remove);
    return row;
  });
  $('queue').replaceChildren(...rows);
  createIcons({ icons });
}

function addFiles(selection) {
  const added = Array.from(selection)
    .filter(file => file.type.startsWith('video/') || /\.(mp4|m4v|mkv|avi|mov|webm|ogv|mpeg|mpg)$/i.test(file.name))
    .map(file => ({ file, url: URL.createObjectURL(file), settings: { ...defaultSettings } }));
  if (!added.length) {
    status('Aucun fichier vidéo reconnu.', true);
    return;
  }
  playlist.push(...added);
  if (!current) load(added[0]);
  else renderPlaylist();
}

function load(item) {
  if (current === item) {
    if (item) play();
    return;
  }
  current = item;
  video.pause();
  scene?.clearFrame();
  screen.classList.toggle('loaded', Boolean(item));
  $('empty').hidden = Boolean(item);
  if (!scene) video.hidden = !item;
  if (item) video.src = item.url;
  else video.removeAttribute('src');
  video.load();
  status();
  refreshSettings();
  refreshTransport();
  renderPlaylist();
  if (item) play();
}

async function play() {
  const item = current;
  try {
    await video.play();
  } catch (error) {
    if (item !== current || error.name === 'AbortError' || video.error) return;
    status(error.name === 'NotAllowedError'
      ? 'Appuie sur Lire pour démarrer la vidéo.'
      : 'Le navigateur n’a pas pu démarrer cette vidéo.', true);
  }
}

function seek(time) {
  if (Number.isFinite(time) && Number.isFinite(video.duration) && video.duration > 0) {
    video.currentTime = Math.max(0, Math.min(video.duration, time));
  }
}

function next() {
  const item = current && playlist[playlist.indexOf(current) + 1];
  if (item) load(item);
}

function configure(name, value) {
  if (current) current.settings[name] = value;
  else defaultSettings[name] = value;
  refreshSettings();
  refreshTransport();
}

function action(name, value) {
  switch (name) {
    case 'play':
      if (current && video.paused) play();
      else video.pause();
      break;
    case 'back': seek(video.currentTime - 10); break;
    case 'forward': seek(video.currentTime + 10); break;
    case 'seek': seek(value * video.duration); break;
    case 'mute':
      if (video.volume === 0) {
        video.volume = 1;
        video.muted = false;
      } else {
        video.muted = !video.muted;
      }
      break;
    case 'reset': scene?.recenter(); break;
    case 'next': next(); break;
    case 'show': scene?.setHud(true); break;
    case 'hide': scene?.setHud(false); break;
    case 'exit': if (session) toggleXR(); break;
    case 'projection': configure('projection', projections[(projections.indexOf(state().projection) + 1) % projections.length]); break;
    case 'layout': configure('layout', layouts[(layouts.indexOf(state().layout) + 1) % layouts.length]); break;
  }
}

async function detectXR() {
  const label = $('xr-status');
  if (!scene) {
    label.textContent = 'WebGL est indisponible. La lecture vidéo standard reste accessible.';
    return;
  }
  if (!window.isSecureContext) {
    label.textContent = 'Ouvre le lecteur en HTTPS ou sur localhost pour activer la VR.';
    return;
  }
  try {
    xrSupported = Boolean(await navigator.xr?.isSessionSupported('immersive-vr'));
  } catch {
    xrSupported = false;
  }
  label.textContent = xrSupported
    ? 'Casque VR détecté. Ouvre une vidéo pour entrer en VR.'
    : 'Aucun casque WebXR détecté. Pour le Rift CV1, ouvre l’application Oculus sur le PC, puis relance Chrome ou Edge.';
  refreshTransport();
}

async function toggleXR() {
  if (session) {
    await session.end().catch(() => {});
    return;
  }
  if (!xrSupported || entering || !current) return;
  entering = true;
  $('xr-status').textContent = 'Connexion au casque… Autorise la VR si le navigateur te le demande.';
  refreshTransport();
  let requested;
  try {
    requested = await navigator.xr.requestSession('immersive-vr');
    session = requested;
    session.addEventListener('end', () => {
      session = null;
      refreshTransport();
      detectXR();
    }, { once: true });
    await scene.start(requested);
    if (session === requested) $('xr-status').textContent = 'Session VR active.';
  } catch (error) {
    if (session === requested && requested) await requested.end().catch(() => {});
    $('xr-status').textContent = `Impossible d’ouvrir la session VR (${error.name}).`;
  } finally {
    entering = false;
    refreshTransport();
  }
}

try {
  scene = createVideoScene($('render'), video, state, action);
} catch {
  $('render').replaceChildren(video);
  $('projection').closest('fieldset').disabled = true;
  $('stereo').disabled = true;
}
for (const id of ['open', 'empty-open']) $(id).addEventListener('click', () => files.click());
files.addEventListener('change', () => {
  if (files.files.length) addFiles(files.files);
  files.value = '';
});
for (const id of ['play', 'back', 'forward', 'mute', 'reset']) $(id).addEventListener('click', () => action(id));
$('seek').addEventListener('input', event => seek(Number(event.target.value)));
$('volume').addEventListener('input', event => {
  video.volume = Number(event.target.value);
  video.muted = false;
});
$('fullscreen').addEventListener('click', async () => {
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else await screen.closest('.player').requestFullscreen();
  } catch {
    status('Le plein écran est indisponible dans ce navigateur.', true);
  }
});
$('projection').addEventListener('click', event => {
  if (event.target.dataset.value) configure('projection', event.target.dataset.value);
});
$('stereo').addEventListener('change', event => configure('layout', event.target.value));
$('swap').addEventListener('change', event => configure('swap', event.target.checked));
$('vr').addEventListener('click', toggleXR);

screen.addEventListener('dragover', event => {
  if (!event.dataTransfer.types.includes('Files')) return;
  event.preventDefault();
  screen.classList.add('dragging');
});
screen.addEventListener('dragleave', event => {
  if (!screen.contains(event.relatedTarget)) screen.classList.remove('dragging');
});
screen.addEventListener('drop', event => {
  event.preventDefault();
  screen.classList.remove('dragging');
  if (event.dataTransfer.files.length) addFiles(event.dataTransfer.files);
});
document.addEventListener('dragover', event => {
  if (event.dataTransfer.types.includes('Files')) event.preventDefault();
});
document.addEventListener('drop', event => {
  if (event.dataTransfer.types.includes('Files')) event.preventDefault();
});

for (const name of ['durationchange', 'timeupdate', 'play', 'pause', 'volumechange']) {
  video.addEventListener(name, refreshTransport);
}
video.addEventListener('loadeddata', () => {
  if (current) scene?.prepareFrame();
});
video.addEventListener('playing', () => status());
video.addEventListener('ended', next);
video.addEventListener('error', () => {
  if (!current) return;
  scene?.clearFrame();
  status('Ce fichier ne peut pas être lu. Essaie un MP4 encodé en H.264 avec audio AAC.', true);
  refreshTransport();
});
document.addEventListener('keydown', event => {
  if (!current || event.altKey || event.ctrlKey || event.metaKey ||
      event.target.closest('button, input, select, a, textarea, [contenteditable]')) return;
  const keys = { ' ': 'play', ArrowLeft: 'back', ArrowRight: 'forward', m: 'mute', r: 'reset' };
  const shortcut = keys[event.key];
  if (!shortcut) return;
  event.preventDefault();
  action(shortcut);
});

createIcons({ icons });
refreshSettings();
refreshTransport();
detectXR();
navigator.xr?.addEventListener('devicechange', detectXR);
