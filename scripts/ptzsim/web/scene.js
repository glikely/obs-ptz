// A 3D scene for the camera to look at, drawn with three.js: loaded by
// index.html only when ptzsim has a --scene, so the page works without it.
import * as THREE from "three";
import { GLTFLoader } from "three/addons/loaders/GLTFLoader.js";
import { RoomEnvironment } from "three/addons/environments/RoomEnvironment.js";

// `view()` is the camera now: {pan, tilt, zoom} normalized, and its angles
export async function start({ url, cameraPos, view }) {
  const renderer = new THREE.WebGLRenderer({ antialias: true });
  renderer.toneMapping = THREE.ACESFilmicToneMapping;
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
  const camera = new THREE.PerspectiveCamera(60, 1, 0.05, 1000);
  camera.rotation.order = "YXZ";
  // By default a camera is mounted high: a third of the way up the scene
  camera.position.set(...(cameraPos || [middle.x, box.min.y + 0.35 * (box.max.y - box.min.y), middle.z]));
  console.log("scene", url, "box", box.min.toArray(), box.max.toArray(), "camera", camera.position.toArray());

  renderer.setAnimationLoop(() => {
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const w = innerWidth, h = innerHeight;
    if (el.width !== Math.round(w * dpr) || el.height !== Math.round(h * dpr)) renderer.setSize(w, h, false), renderer.setPixelRatio(dpr);
    const v = view();
    camera.aspect = w / h;
    // The page's field of view is the horizontal one; three.js wants the vertical
    camera.fov = 2 * Math.atan(Math.tan(v.hfov / 2) / camera.aspect) * 180 / Math.PI;
    camera.updateProjectionMatrix();
    camera.rotation.set(v.pitch, -v.yaw, 0);   // pan right is a turn to the right
    renderer.render(scene, camera);
  });
}
