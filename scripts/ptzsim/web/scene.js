// A 3D scene for the camera to look at, drawn with three.js: loaded by
// index.html only when ptzsim has a --scene, so the page works without it.
import * as THREE from "three";
import { GLTFLoader } from "three/addons/loaders/GLTFLoader.js";
import { RoomEnvironment } from "three/addons/environments/RoomEnvironment.js";

// `view()` is the camera now: its yaw, pitch and horizontal field of view in
// radians, and its focus (0..1). `grid` is the canvas of degree lines
// (equirectangular, transparent) to lay over the picture, or null.
export async function start({ url, cameraPos, view, grid, exposure = 1 }) {
  const renderer = new THREE.WebGLRenderer({ antialias: true });
  renderer.toneMapping = THREE.ACESFilmicToneMapping;
  // How bright the scene is: some come unlit or dark; ?exposure= tries one
  renderer.toneMappingExposure = Number(new URLSearchParams(location.search).get("exposure")) || exposure;
  renderer.autoClear = false;
  const el = renderer.domElement;
  // Over the page's own canvas, which still takes the mouse for dragging
  Object.assign(el.style, { position: "fixed", inset: "0", width: "100vw", height: "100vh",
                            zIndex: "1", pointerEvents: "none" });
  document.body.appendChild(el);

  const scene = new THREE.Scene();
  const pmrem = new THREE.PMREMGenerator(renderer);
  scene.environment = pmrem.fromScene(new RoomEnvironment(), 0.04).texture;
  scene.background = new THREE.Color(0x202428);
  const sun = new THREE.DirectionalLight(0xffffff, 2);
  sun.position.set(3, 10, 4);
  scene.add(sun, new THREE.HemisphereLight(0xffffff, 0x666655, 0.8));

  const gltf = await new GLTFLoader().loadAsync(url);
  scene.add(gltf.scene);
  const box = new THREE.Box3().setFromObject(gltf.scene);
  const middle = box.getCenter(new THREE.Vector3());
  const camera = new THREE.PerspectiveCamera(60, 1, 0.1, 1000);
  camera.rotation.order = "YXZ";
  // By default a camera is mounted high: a third of the way up the scene
  camera.position.set(...(cameraPos || [middle.x, box.min.y + 0.35 * (box.max.y - box.min.y), middle.z]));
  console.log("scene", url, "box", box.min.toArray(), box.max.toArray(), "camera", camera.position.toArray());

  camera.near = 0.1;
  camera.far = 1000;

  // The degree lines: a sphere around the camera, drawn over everything. The
  // grid's longitude 0 is straight ahead (-Z here) and grows to the right.
  const gridScene = new THREE.Scene();
  let gridSphere = null;
  if (grid) {
    const texture = new THREE.CanvasTexture(grid);
    texture.colorSpace = THREE.SRGBColorSpace;
    texture.anisotropy = renderer.capabilities.getMaxAnisotropy();
    gridSphere = new THREE.Mesh(
      new THREE.SphereGeometry(1, 96, 48),
      new THREE.MeshBasicMaterial({ map: texture, transparent: true, side: THREE.BackSide,
                                    depthTest: false, depthWrite: false, toneMapped: false }));
    gridSphere.scale.x = -1;                    // as seen from inside, u would run the wrong way
    gridSphere.rotation.y = -Math.PI / 2;
    gridScene.add(gridSphere);
  }

  // Focus: the scene is drawn to a target with its depth, then blurred by how
  // far each pixel's distance is from the focus distance, as a lens would.
  // Focus 0 is 0.5 m and 1 is 100 m; the blur grows with the zoom.
  // A target and its depth texture are made again at each size: resizing
  // one in place leaves the depth texture at its old size
  let target = null;
  const makeTarget = (w, h) => {
    if (target) { target.depthTexture.dispose(); target.dispose(); }
    target = new THREE.WebGLRenderTarget(w, h, { type: THREE.HalfFloatType, depthTexture: new THREE.DepthTexture(w, h, THREE.FloatType) });
    blur.uniforms.tColor.value = target.texture;
    blur.uniforms.tDepth.value = target.depthTexture;
    blur.uniforms.size.value.set(w, h);
  };
  const blur = new THREE.ShaderMaterial({
    uniforms: { tColor: { value: null }, tDepth: { value: null },
                size: { value: new THREE.Vector2() }, near: { value: camera.near }, far: { value: camera.far },
                focusDist: { value: 10 }, cocScale: { value: 0 } },
    vertexShader: "varying vec2 uv_; void main() { uv_ = uv; gl_Position = vec4(position.xy, 0., 1.); }",
    fragmentShader: `
      varying vec2 uv_;
      uniform sampler2D tColor, tDepth;
      uniform vec2 size;
      uniform float near, far, focusDist, cocScale;
      float distanceAt(vec2 uv) {
        float z = texture2D(tDepth, uv).x * 2. - 1.;
        return 2. * near * far / (far + near - z * (far - near));
      }
      float cocAt(vec2 uv) { return min(cocScale * abs(1. / distanceAt(uv) - 1. / focusDist), 24.); }
      void main() {
        float coc = cocAt(uv_);
        vec3 sum = texture2D(tColor, uv_).rgb;
        float weight = 1.;
        if (coc > 0.5) {
          for (int i = 1; i < 40; i++) {            // a golden-angle spiral over the disc
            float r = sqrt(float(i) / 40.), a = float(i) * 2.39996;
            vec2 offset = vec2(cos(a), sin(a)) * r * coc;
            vec2 uv = uv_ + offset / size;
            // A sample only blurs into this pixel if its own blur reaches this far
            float w = clamp(cocAt(uv) - length(offset) + 1., 0., 1.);
            sum += texture2D(tColor, uv).rgb * w;
            weight += w;
          }
        }
        gl_FragColor = vec4(sum / weight, 1.);
        #include <tonemapping_fragment>
        #include <colorspace_fragment>
      }`,
    depthTest: false, depthWrite: false,
  });
  const quad = new THREE.Mesh(new THREE.PlaneGeometry(2, 2), blur);
  quad.frustumCulled = false;
  const quadScene = new THREE.Scene();
  quadScene.add(quad);
  const quadCamera = new THREE.OrthographicCamera(-1, 1, 1, -1, 0, 1);

  renderer.setAnimationLoop(() => {
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const w = innerWidth, h = innerHeight;
    if (el.width !== Math.round(w * dpr) || el.height !== Math.round(h * dpr)) {
      renderer.setPixelRatio(dpr);
      renderer.setSize(w, h, false);
      makeTarget(el.width, el.height);
    }
    const v = view();
    camera.aspect = w / h;
    // The page's field of view is the horizontal one; three.js wants the vertical
    camera.fov = 2 * Math.atan(Math.tan(v.hfov / 2) / camera.aspect) * 180 / Math.PI;
    camera.updateProjectionMatrix();
    camera.rotation.set(v.pitch, -v.yaw, 0);   // pan right is a turn to the right
    if (gridSphere) gridSphere.position.copy(camera.position);

    // About 4 px of blur per 1/m of defocus at a 720p picture, growing
    // with the zoom as a longer lens's depth of field shrinks
    const zoomed = Math.tan(70 * Math.PI / 360) / Math.tan(v.hfov / 2);
    blur.uniforms.focusDist.value = 0.5 * Math.pow(200, v.focus);
    // ?dof=0 turns the blur off
    blur.uniforms.cocScale.value = new URLSearchParams(location.search).get("dof") === "0" ? 0 : 4 * zoomed * el.height / 720;

    renderer.setRenderTarget(target);
    renderer.clear();
    renderer.render(scene, camera);
    renderer.setRenderTarget(null);
    renderer.clear();
    renderer.render(quadScene, quadCamera);
    renderer.render(gridScene, camera);
  });
}
