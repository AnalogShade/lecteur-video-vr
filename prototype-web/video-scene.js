import {
  Mesh, PerspectiveCamera, PlaneGeometry, Scene, ShaderMaterial, SphereGeometry, Vector3, Vector4,
  VideoTexture, WebGLRenderer,
} from 'three';
import { displayAspect, eyeRegion } from './media-layout.js';
import { createHud } from './xr-hud.js';

const vertexShader = `
  varying vec2 texCoord;
  varying vec3 direction;
  void main() {
    texCoord = uv;
    direction = position;
    gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
  }
`;

const fragmentShader = `
  uniform sampler2D frame;
  uniform vec4 region;
  uniform vec2 frameSize;
  uniform float angle;
  varying vec2 texCoord;
  varying vec3 direction;
  const float PI = 3.141592653589793;
  void main() {
    vec2 uv = texCoord;
    if (angle > 0.0) {
      vec3 facing = normalize(direction);
      float longitude = atan(facing.x, -facing.z);
      uv = vec2(0.5 + longitude / angle, 0.5 + asin(clamp(facing.y, -1.0, 1.0)) / PI);
      if (uv.x < 0.0 || uv.x > 1.0) {
        gl_FragColor = vec4(0.031, 0.035, 0.059, 1.0);
        return;
      }
    }
    // Keep linear filtering from sampling the neighboring eye at a packed frame's edge.
    vec2 inset = 0.5 / frameSize;
    vec2 sampleAt = clamp(uv * region.xy + region.zw, region.zw + inset, region.zw + region.xy - inset);
    gl_FragColor = vec4(texture2D(frame, sampleAt).rgb, 1.0);
  }
`;

export function createVideoScene(container, video, getState, onAction) {
  const renderer = new WebGLRenderer({ antialias: true, alpha: false });
  renderer.setClearColor(0x080911);
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
  renderer.xr.enabled = true;
  renderer.xr.setReferenceSpaceType('local');
  container.append(renderer.domElement);

  const scene = new Scene();
  const camera = new PerspectiveCamera(67, 1, 0.04, 50);
  camera.rotation.order = 'YXZ';
  scene.add(camera);
  const uniforms = {
    frame: { value: null },
    region: { value: new Vector4() },
    frameSize: { value: { x: 1, y: 1 } },
    angle: { value: 0 },
  };
  const material = new ShaderMaterial({ uniforms, vertexShader, fragmentShader });
  const screen = new Mesh(new PlaneGeometry(), material);
  // Invert the sphere's winding so the texture is visible from inside.
  const panorama = new Mesh(new SphereGeometry(18, 64, 40).scale(-1, 1, 1), material);
  screen.visible = false;
  panorama.visible = false;
  scene.add(screen, panorama);
  const hud = createHud(scene, renderer, getState, onAction);
  const head = new Vector3();
  const forward = new Vector3(0, 0, -1);
  const anchor = new Vector3();
  const hudOffset = new Vector3();
  const up = new Vector3(0, 1, 0);
  let yaw = 0;
  let pitch = 0;
  let recenterPending = true;
  let hudPlacementPending = true;
  let rightEye;
  let active = false;
  let frameReady = false;
  let projection = 'flat';
  let layout = 'mono';
  let swap = false;

  function setEye(_renderer, _scene, view) {
    const eye = view === rightEye ? 1 : 0;
    uniforms.region.value.fromArray(eyeRegion(layout, eye, swap));
    material.uniformsNeedUpdate = true;
  }
  screen.onBeforeRender = setEye;
  panorama.onBeforeRender = setEye;

  function resize() {
    if (renderer.xr.isPresenting) return;
    const { clientWidth: width, clientHeight: height } = container;
    if (!width || !height) return;
    camera.aspect = width / height;
    camera.updateProjectionMatrix();
    renderer.setSize(width, height, false);
  }
  new ResizeObserver(resize).observe(container);
  resize();

  function placeHud(heading) {
    hudOffset.set(0, -0.63, -1.85).applyAxisAngle(up, heading);
    hud.group.position.copy(head).add(hudOffset);
    hud.group.rotation.y = heading;
    hudPlacementPending = false;
  }

  let drag;
  const canvas = renderer.domElement;
  canvas.addEventListener('pointerdown', event => {
    if (!active || projection === 'flat' || event.button !== 0) return;
    canvas.setPointerCapture(event.pointerId);
    drag = { id: event.pointerId, x: event.clientX, y: event.clientY };
  });
  canvas.addEventListener('pointermove', event => {
    if (drag?.id !== event.pointerId) return;
    yaw += (event.clientX - drag.x) * 0.005;
    pitch = Math.max(-Math.PI / 2, Math.min(Math.PI / 2, pitch + (event.clientY - drag.y) * 0.005));
    drag = { id: event.pointerId, x: event.clientX, y: event.clientY };
  });
  canvas.addEventListener('lostpointercapture', () => { drag = null; });

  renderer.xr.addEventListener('sessionend', () => {
    hud.group.visible = false;
    camera.position.set(0, 0, 0);
    camera.rotation.set(0, 0, 0);
    yaw = 0;
    pitch = 0;
    resize();
  });

  renderer.setAnimationLoop((_time, frame) => {
    const immersive = renderer.xr.isPresenting;
    if (!active && !immersive) return;
    if (immersive) {
      const pose = frame.getViewerPose(renderer.xr.getReferenceSpace());
      if (!pose) return;
      renderer.xr.updateCamera(camera);
      const view = renderer.xr.getCamera();
      rightEye = view.cameras[pose.views.findIndex(eye => eye.eye === 'right')];
      view.getWorldPosition(head);
      if (recenterPending || hudPlacementPending) {
        forward.set(0, 0, -1).transformDirection(view.matrixWorld);
        const heading = Math.hypot(forward.x, forward.z) > 0.001 ? Math.atan2(-forward.x, -forward.z) : yaw;
        if (recenterPending) {
          yaw = heading;
          anchor.copy(head);
          recenterPending = false;
        }
        placeHud(heading);
      }
      panorama.position.copy(head);
      panorama.rotation.y = yaw;
      screen.position.set(0, 0.16, -3.2).applyAxisAngle(up, yaw).add(anchor);
      screen.rotation.y = yaw;
      scene.updateMatrixWorld();
      hud.update();
    } else {
      rightEye = undefined;
      camera.rotation.set(projection === 'flat' ? 0 : pitch, projection === 'flat' ? 0 : yaw, 0, 'YXZ');
      panorama.position.set(0, 0, 0);
      panorama.rotation.y = 0;
      screen.position.set(0, 0, -3.2);
      screen.rotation.y = 0;
    }

    if (frameReady && video.readyState >= HTMLMediaElement.HAVE_CURRENT_DATA) {
      const aspect = displayAspect(video.videoWidth, video.videoHeight, layout);
      const maxHeight = immersive ? 2.15 : 6.4 * Math.tan(camera.fov * Math.PI / 360);
      const maxWidth = immersive ? 4.4 : maxHeight * camera.aspect;
      const height = Math.min(maxHeight, maxWidth / aspect);
      screen.scale.set(height * aspect, height, 1);
      uniforms.frameSize.value.x = video.videoWidth || 1;
      uniforms.frameSize.value.y = video.videoHeight || 1;
    }
    screen.visible = active && frameReady && projection === 'flat';
    panorama.visible = active && frameReady && projection !== 'flat';
    uniforms.angle.value = projection === 'flat' ? 0 : Number(projection) * Math.PI / 180;
    renderer.render(scene, camera);
  });

  return {
    update(options) {
      ({ active, projection, layout, swap } = options);
      canvas.hidden = !active;
    },
    clearFrame() {
      frameReady = false;
    },
    prepareFrame() {
      // A new file may have a different resolution, so create a fresh GPU texture.
      uniforms.frame.value?.dispose();
      uniforms.frame.value = new VideoTexture(video);
      frameReady = true;
    },
    recenter() {
      if (renderer.xr.isPresenting) recenterPending = true;
      else {
        yaw = 0;
        pitch = 0;
      }
    },
    setHud(visible) {
      hud.group.visible = visible;
      if (visible) hudPlacementPending = true;
    },
    async start(session) {
      recenterPending = true;
      hud.group.visible = true;
      await renderer.xr.setSession(session);
    },
  };
}
