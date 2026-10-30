// Procedural 3D model of the Sipeed SLogic16U3, after the product photos: black anodized aluminum case
// (a single sleeve sliding over the board, "5Gbps / SLogic" logo, and two end caps), 2x10 female header
// (CH0-CH15, GND, CK) in the front cap, USB-C, LED and MODE button in the back cap, and the board inside: black soldermask with
// yellow edges, Gowin GW5AT-15 (MBGA132), VL162 Type-C mux (QFN), SPI Flash, 125MHz oscillator, 8-pin JST
// SH JTAG/UART connector (5V/GND) on the left edge, input dividers arrays and the right angle header legs,
// "SLogic 16 U3" / SIPEED silkscreen. Board layout from a perspective corrected top photo (board
// coordinates u: left to right, v: back/USB-C to front/header). 1 world unit = 10 mm.
import * as THREE from 'three';

export const SIZE = 4.0, HEIGHT = 1.0;
const BS = 3.7, BT = 0.08;   // board size/thickness

function canvas(w, h) { const c = document.createElement('canvas'); c.width = w; c.height = h; return c; }
function tex(c) { const t = new THREE.CanvasTexture(c); t.colorSpace = THREE.SRGBColorSpace; t.anisotropy = 16; return t; }
function rng(seed) { return () => { seed |= 0; seed = seed + 0x6d2b79f5 | 0; let t = Math.imul(seed ^ seed >>> 15, 1 | seed); t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t; return ((t ^ t >>> 14) >>> 0) / 4294967296; }; }

// Molded package with a laser marked top (canvas texture).
function chip(w, h, d, lines = [], { color = 0x111214, ink = 'rgba(190,196,204,0.55)', font = 40, sub = 30, pins = null, substrate = false } = {}) {
  const g = new THREE.Group();
  const c = canvas(512, Math.max(64, Math.round(512 * d / w)));
  const x = c.getContext('2d');
  x.fillStyle = '#141518'; x.fillRect(0, 0, c.width, c.height);
  x.fillStyle = 'rgba(255,255,255,0.10)'; x.beginPath(); x.arc(c.width * 0.09, c.height * 0.12, c.width * 0.025, 0, 7); x.fill();
  x.fillStyle = ink; x.textAlign = 'center'; x.textBaseline = 'middle';
  lines.forEach((l, i) => {
    x.font = `${i ? 400 : 700} ${i ? sub : font}px Ubuntu`;
    x.fillText(l, c.width / 2, c.height / 2 + (i - (lines.length - 1) / 2) * font * 1.2);
  });
  const top = new THREE.MeshStandardMaterial({ map: tex(c), roughness: 0.62, metalness: 0.05 });
  const side = new THREE.MeshStandardMaterial({ color, roughness: 0.65 });
  let y0 = 0;
  if (substrate) {   // BGA: laminate substrate under the mold cap
    const s = new THREE.Mesh(new THREE.BoxGeometry(w, 0.025, d), new THREE.MeshStandardMaterial({ color: 0x1d2a1f, roughness: 0.5 }));
    s.position.y = 0.0125; g.add(s); y0 = 0.025; w *= 0.94; d *= 0.94;
  }
  const m = new THREE.Mesh(new THREE.BoxGeometry(w, h, d), [side, side, top, side, side, side]);
  m.position.y = y0 + h / 2; m.castShadow = true; g.add(m);
  if (pins) {   // gull wing (SOP) or bottom (QFN, short) terminations on the long sides
    const pm = new THREE.MeshStandardMaterial({ color: 0xc4c7cc, metalness: 0.9, roughness: 0.3 });
    for (const s of [-1, 1]) for (let i = 0; i < pins; i++) {
      const p = new THREE.Mesh(new THREE.BoxGeometry(w / pins * 0.45, 0.02, 0.07), pm);
      p.position.set(-w / 2 + w * (i + 0.5) / pins, 0.01, s * (d / 2 + 0.03)); g.add(p);
    }
  }
  return g;
}

// Lid top: "5Gbps" square wave into a probe, "SLogic" (product logo), pin 1 mark, chamfer highlight.
function labelTexture() {
  const N = 2048, c = canvas(N, N), x = c.getContext('2d');
  x.clearRect(0, 0, N, N);
  // Diamond cut chamfer: light edge line.
  x.strokeStyle = 'rgba(205,210,216,0.55)'; x.lineWidth = 7; x.strokeRect(5, 5, N - 10, N - 10);
  x.strokeStyle = 'rgba(205,210,216,0.18)'; x.lineWidth = 3; x.strokeRect(16, 16, N - 32, N - 32);
  // Pin 1 mark (front edge, CK/odd row end).
  x.fillStyle = 'rgba(232,234,238,0.9)';
  x.beginPath(); x.moveTo(330, N - 70); x.lineTo(366, N - 70); x.lineTo(348, N - 102); x.fill();
  // Logo, reading toward the header (front), letters up to the right side.
  x.save(); x.translate(N / 2, N / 2); x.rotate(Math.PI / 2); x.translate(-N / 2, -N / 2);
  x.fillStyle = x.strokeStyle = 'rgba(236,238,241,0.95)';
  x.lineWidth = 14; x.lineJoin = 'miter'; x.lineCap = 'butt';
  const bx = 430, by = 1010;
  x.font = '700 70px Ubuntu'; x.textAlign = 'left'; x.fillText('5Gbps', bx + 10, by - 300);
  // Square wave: underline of "5Gbps", one wide pulse, three narrow ones, then down to the probe.
  x.beginPath();
  x.moveTo(bx, by - 270); x.lineTo(bx + 250, by - 270); x.lineTo(bx + 250, by - 360); x.lineTo(bx + 420, by - 360);
  x.lineTo(bx + 420, by - 170);
  for (let i = 0; i < 3; i++) { const p = bx + 420 + i * 90; x.lineTo(p + 45, by - 170); x.lineTo(p + 45, by - 330); x.lineTo(p + 90, by - 330); x.lineTo(p + 90, by - 170); }
  x.lineTo(bx + 760, by - 170); x.lineTo(bx + 760, by - 120); x.lineTo(bx + 830, by - 120);
  x.stroke();
  x.font = '700 380px Ubuntu'; x.textBaseline = 'alphabetic';
  x.fillText('SLog', bx - 20, by + 230);
  const wi = x.measureText('SLog').width;
  x.fillText('c', bx - 20 + wi + 120, by + 230);
  // Probe as the "i": tip on the baseline, body and cable up to the right.
  x.save(); x.translate(bx - 20 + wi + 70, by + 240); x.rotate(-0.42);
  x.fillRect(-8, -40, 16, 40);                 // tip
  x.beginPath(); x.moveTo(-34, -60); x.lineTo(34, -60); x.lineTo(20, -40); x.lineTo(-20, -40); x.fill();
  x.fillRect(-34, -330, 68, 270);              // body
  x.clearRect(-12, -300, 24, 200);             // grip slot
  x.fillRect(-26, -380, 52, 50);               // collar
  x.fillRect(-9, -620, 18, 240);               // cable
  x.restore();
  x.restore();
  return tex(c);
}

// Case front: female header face (2x10 sockets) and the channel labels around it.
function headerFaceTexture() {
  const c = canvas(1024, 160), x = c.getContext('2d');
  x.fillStyle = '#0b0c0e'; x.fillRect(0, 0, 1024, 160);
  for (let r = 0; r < 2; r++) for (let i = 0; i < 10; i++) {
    const cx = 51 + i * 102.4, cy = 46 + r * 68;
    x.fillStyle = '#020203'; x.fillRect(cx - 16, cy - 16, 32, 32);
    x.strokeStyle = 'rgba(120,124,130,0.35)'; x.lineWidth = 3; x.strokeRect(cx - 18, cy - 18, 36, 36);
  }
  return tex(c);
}
function pinLabelsTexture(labels) {
  const c = canvas(1024, 64), x = c.getContext('2d');
  x.clearRect(0, 0, 1024, 64);
  x.fillStyle = 'rgba(232,234,238,0.9)'; x.font = '700 40px Ubuntu'; x.textAlign = 'center'; x.textBaseline = 'middle';
  labels.forEach((l, i) => x.fillText(l, 51 + i * 102.4, 34));
  return tex(c);
}

// Board top: black soldermask, yellow edge, silkscreen, pads, traces.
function boardTexture() {
  const N = 2048, c = canvas(N, N), x = c.getContext('2d');
  const U = u => u * N;
  x.fillStyle = '#c9a23a'; x.fillRect(0, 0, N, N);                   // yellow edge (laminate)
  x.fillStyle = '#0c0d0f'; x.fillRect(U(0.022), U(0.022), N - 2 * U(0.022), N - 2 * U(0.022));
  // Subtle copper pours/traces under the mask.
  x.strokeStyle = 'rgba(255,255,255,0.035)'; x.lineWidth = 6;
  const r = rng(7);
  for (let i = 0; i < 70; i++) {
    const u0 = 0.1 + 0.8 * r(), v0 = 0.1 + 0.8 * r();
    x.beginPath(); x.moveTo(U(u0), U(v0)); x.lineTo(U(u0 + (r() - 0.5) * 0.25), U(v0)); x.lineTo(U(u0 + (r() - 0.5) * 0.25), U(v0 + (r() - 0.5) * 0.25)); x.stroke();
  }
  // Input traces: header legs -> dividers -> FPGA.
  x.strokeStyle = 'rgba(255,255,255,0.05)'; x.lineWidth = 5;
  for (let i = 0; i < 16; i++) {
    const ux = 0.12 + i * 0.047;
    x.beginPath(); x.moveTo(U(ux), U(0.84)); x.lineTo(U(ux), U(0.70)); x.lineTo(U(0.56 + i * 0.012), U(0.56)); x.stroke();
  }
  // Header legs pads (2 rows of 10, gold).
  x.fillStyle = '#d9b45c';
  for (let rr = 0; rr < 2; rr++) for (let i = 0; i < 10; i++) {
    const cx = U(0.098 + i * 0.0828), cy = U(rr ? 0.792 : 0.842);
    x.beginPath(); x.arc(cx, cy, U(0.017), 0, 7); x.fill();
  }
  // Fine pitch pads/vias around the FPGA and mux.
  x.fillStyle = 'rgba(200,170,90,0.35)';
  for (let i = 0; i < 120; i++) { x.beginPath(); x.arc(U(0.48 + 0.4 * r()), U(0.18 + 0.42 * r()), 4, 0, 7); x.fill(); }
  // Silkscreen.
  x.fillStyle = x.strokeStyle = 'rgba(238,240,242,0.92)';
  const rot = (u, v, f) => { x.save(); x.translate(U(u), U(v)); x.rotate(Math.PI / 2); f(); x.restore(); };
  rot(0.935, 0.07, () => {   // "SLogic [16] U3" along the right edge
    x.font = '700 92px Ubuntu'; x.textBaseline = 'middle'; x.textAlign = 'left';
    x.fillText('SLogic', 0, 0);
    let w = x.measureText('SLogic').width + 22;
    x.lineWidth = 7; x.strokeRect(w, -50, 110, 100); x.font = '700 78px Ubuntu'; x.fillText('16', w + 12, 4);
    x.font = '700 92px Ubuntu'; x.fillText('U3', w + 170, 0);
    x.beginPath(); x.arc(-60, 0, 9, 0, 7); x.fill(); x.beginPath(); x.arc(-90, 0, 9, 0, 7); x.fill();
  });
  rot(0.44, 0.33, () => {   // SIPEED logo
    x.lineWidth = 6; x.fillRect(0, -44, 88, 88);
    x.fillStyle = '#0c0d0f'; x.font = '700 76px Ubuntu'; x.textAlign = 'center'; x.textBaseline = 'middle'; x.fillText('S', 44, 4);
    x.fillStyle = 'rgba(238,240,242,0.92)'; x.textAlign = 'left'; x.font = '700 92px Ubuntu'; x.fillText('SIPEED', 106, 4);
  });
  x.font = '700 40px Ubuntu'; x.textAlign = 'center'; x.textBaseline = 'middle';
  x.fillText('5V', U(0.165), U(0.17)); x.fillText('GND', U(0.165), U(0.50));
  // Component outlines (silkscreen).
  x.lineWidth = 4;
  x.strokeRect(U(0.735), U(0.04), U(0.15), U(0.15));      // flash
  x.strokeRect(U(0.23), U(0.27), U(0.16), U(0.16));       // mux
  x.strokeRect(U(0.03), U(0.15), U(0.11), U(0.37));       // JST
  return tex(c);
}

export function buildDevice() {
  const root = new THREE.Group();
  const anchors = {};

  const alu = new THREE.MeshPhysicalMaterial({ color: 0x0c0d0f, metalness: 0.25, roughness: 0.72, clearcoat: 0.05, clearcoatRoughness: 0.6 });
  alu.envMapIntensity = 0.35;   // deep black anodized (studio reflections kept subtle)

  // Case: a single extruded sleeve sliding over the board (logo on top) and two end caps: front (2x10
  // header opening, pin labels) and back (USB-C, LED, MODE button).
  const L = SIZE - 0.2, CT = 0.1, R = 0.12, WT = 0.08; // Sleeve length, caps/walls thicknesses.
  const rrect = (path, x0, y0, x1, y1, r) => {
    path.moveTo(x0 + r, y0); path.lineTo(x1 - r, y0); path.quadraticCurveTo(x1, y0, x1, y0 + r);
    path.lineTo(x1, y1 - r); path.quadraticCurveTo(x1, y1, x1 - r, y1); path.lineTo(x0 + r, y1);
    path.quadraticCurveTo(x0, y1, x0, y1 - r); path.lineTo(x0, y0 + r); path.quadraticCurveTo(x0, y0, x0 + r, y0);
    return path;
  };
  const stadium = (path, cx, cy, w, h) => {
    const r = h / 2;
    path.absarc(cx - w / 2 + r, cy, r, Math.PI / 2, 3 * Math.PI / 2);
    path.absarc(cx + w / 2 - r, cy, r, -Math.PI / 2, Math.PI / 2);
    return path;
  };
  const circle = (path, cx, cy, r) => { path.absarc(cx, cy, r, 0, 2 * Math.PI, true); return path; };
  const caseMat = () => { const m = alu.clone(); m.transparent = true; return m; };
  const caseMats = [];

  const sleeve = new THREE.Group(); root.add(sleeve);
  const sleeveMat = caseMat(); caseMats.push(sleeveMat);
  const profile = rrect(new THREE.Shape(), -SIZE / 2, 0, SIZE / 2, HEIGHT, R);
  profile.holes.push(rrect(new THREE.Path(), -SIZE / 2 + WT, WT, SIZE / 2 - WT, HEIGHT - WT, R / 2));
  const tube = new THREE.Mesh(new THREE.ExtrudeGeometry(profile, { depth: L, bevelEnabled: false, curveSegments: 8 }), sleeveMat);
  tube.position.z = -L / 2; tube.castShadow = tube.receiveShadow = true; sleeve.add(tube);
  const labelMat = new THREE.MeshBasicMaterial({ map: labelTexture(), transparent: true, depthWrite: false });
  caseMats.push(labelMat);
  const label = new THREE.Mesh(new THREE.PlaneGeometry(SIZE - 2 * R, L), labelMat);
  label.rotation.x = -Math.PI / 2; label.position.y = HEIGHT + 0.002; sleeve.add(label);

  const hdrMat  = new THREE.MeshStandardMaterial({ color: 0x0b0c0e, roughness: 0.75 });
  const faceMat = new THREE.MeshStandardMaterial({ map: headerFaceTexture(), roughness: 0.8 });
  const hw = 0.82 * BS, hx = (0.47 - 0.5) * BS;
  const ux = (0.47 - 0.5) * BS, uy = 0.62;
  const ledX = (0.05 - 0.5) * BS, modeX = (0.156 - 0.5) * BS;

  // Front cap: header opening, pin labels.
  const capF = new THREE.Group(); root.add(capF);
  const capFMat = caseMat(); caseMats.push(capFMat);
  const front = rrect(new THREE.Shape(), -SIZE / 2, 0, SIZE / 2, HEIGHT, R);
  front.holes.push(rrect(new THREE.Path(), hx - hw / 2 - 0.03, 0.48, hx + hw / 2 + 0.03, 0.90, 0.02));
  const capFMesh = new THREE.Mesh(new THREE.ExtrudeGeometry(front, { depth: CT, bevelEnabled: false }), capFMat);
  capFMesh.position.z = L / 2; capFMesh.castShadow = true; capF.add(capFMesh);
  for (const [labels, y] of [[['CK', 'G', '15', '13', '11', '9', '7', '5', '3', '1'], 0.95],
                             [['CK', 'G', '14', '12', '10', '8', '6', '4', '2', '0'], 0.42]]) {
    const m = new THREE.MeshBasicMaterial({ map: pinLabelsTexture(labels), transparent: true, depthWrite: false });
    caseMats.push(m);
    const l = new THREE.Mesh(new THREE.PlaneGeometry(hw, hw / 16), m);
    l.position.set(hx, y, SIZE / 2 + 0.003); capF.add(l);
  }
  anchors.header = new THREE.Vector3(hx - 0.8, 0.7, SIZE / 2 + 0.05);

  // Back cap: USB-C, LED (light pipe), MODE button.
  const capB = new THREE.Group(); root.add(capB);
  const capBMat = caseMat(); caseMats.push(capBMat);
  const back = rrect(new THREE.Shape(), -SIZE / 2, 0, SIZE / 2, HEIGHT, R);
  back.holes.push(stadium(new THREE.Path(), ux, uy, 1.0, 0.42));
  back.holes.push(circle(new THREE.Path(), ledX, uy, 0.07));
  back.holes.push(circle(new THREE.Path(), modeX, uy, 0.11));
  const capBMesh = new THREE.Mesh(new THREE.ExtrudeGeometry(back, { depth: CT, bevelEnabled: false }), capBMat);
  capBMesh.position.z = -SIZE / 2; capBMesh.castShadow = true; capB.add(capBMesh);
  const shellMat = new THREE.MeshStandardMaterial({ color: 0xc9ccd2, metalness: 0.9, roughness: 0.38 });
  anchors.usb = new THREE.Vector3(ux, uy, -SIZE / 2 - 0.1);
  const ledMat = new THREE.MeshBasicMaterial({ color: 0xff3838 });
  const led = new THREE.Mesh(new THREE.CylinderGeometry(0.06, 0.06, CT, 20), ledMat);
  led.rotation.x = Math.PI / 2; led.position.set(ledX, uy, -SIZE / 2 + CT / 2 - 0.004); capB.add(led);
  const ledGlow = new THREE.Mesh(new THREE.CircleGeometry(0.16, 24), new THREE.MeshBasicMaterial({ color: 0xff3838, transparent: true, opacity: 0.35, depthWrite: false }));
  ledGlow.rotation.y = Math.PI; ledGlow.position.set(ledX, uy, -SIZE / 2 - 0.02); capB.add(ledGlow);
  anchors.led = new THREE.Vector3(ledX, uy, -SIZE / 2 - 0.05);
  const btn = new THREE.Mesh(new THREE.CylinderGeometry(0.09, 0.09, CT + 0.04, 24), shellMat);
  btn.rotation.x = Math.PI / 2; btn.position.set(modeX, uy, -SIZE / 2 + CT / 2 - 0.02); capB.add(btn);
  anchors.mode = new THREE.Vector3(modeX, uy, -SIZE / 2 - 0.05);

  // Board (inside).
  const pcb = new THREE.Group(); pcb.position.y = 0.42; root.add(pcb);
  const pcbTop = new THREE.MeshStandardMaterial({ map: boardTexture(), roughness: 0.45, metalness: 0.1 });
  const pcbSide = new THREE.MeshStandardMaterial({ color: 0xc9a23a, roughness: 0.6 });
  const board = new THREE.Mesh(new THREE.BoxGeometry(BS, BT, BS), [pcbSide, pcbSide, pcbTop, pcbSide, pcbSide, pcbSide]);
  board.position.y = BT / 2; board.receiveShadow = true; pcb.add(board);
  const P = (u, v, y = 0) => new THREE.Vector3((u - 0.5) * BS, BT + y, (v - 0.5) * BS);
  const place = (o, u, v) => { o.position.copy(P(u, v)); pcb.add(o); return o; };

  place(chip(0.88, 0.07, 0.88, ['GOWIN', 'GW5AT-LV15MG132', 'C1/I0'], { font: 64, sub: 40, substrate: true }), 0.665, 0.40);
  anchors.fpga = P(0.665, 0.40, 0.11);
  place(chip(0.5, 0.06, 0.5, ['VL162'], { font: 70 }), 0.31, 0.35);
  anchors.mux = P(0.31, 0.35, 0.07);
  const flash = place(chip(0.5, 0.07, 0.42, ['SPI FLASH'], { font: 60, pins: 4 }), 0.81, 0.115);
  flash.rotation.y = Math.PI / 2;
  anchors.flash = P(0.81, 0.115, 0.08);
  const osc = place(chip(0.32, 0.08, 0.25, ['125.000'], { font: 96, color: 0xb9bdc3 }), 0.47, 0.205);
  anchors.osc = P(0.47, 0.205, 0.09);

  // USB-C receptacle (mid-mount, back edge) and the side MODE switch.
  const rec = new THREE.Mesh(new THREE.BoxGeometry(0.9, 0.32, 0.75), shellMat);
  rec.position.copy(P(0.47, 0.07, 0.12)); pcb.add(rec);
  const recZ = P(0.47, 0.07).z - 0.375; // Receptacle front (in the back cap opening).
  const cavity = new THREE.Mesh(new THREE.BoxGeometry(0.82, 0.24, 0.02), new THREE.MeshBasicMaterial({ color: 0x050506 }));
  cavity.position.set(P(0.47, 0).x, BT + 0.12, recZ - 0.001); pcb.add(cavity);
  const tongue = new THREE.Mesh(new THREE.BoxGeometry(0.5, 0.06, 0.06), new THREE.MeshStandardMaterial({ color: 0x1a1c20 }));
  tongue.position.set(P(0.47, 0).x, BT + 0.12, recZ + 0.02); pcb.add(tongue);
  const sw = new THREE.Mesh(new THREE.BoxGeometry(0.34, 0.2, 0.22), shellMat);
  sw.position.copy(P(0.156, 0.04, 0.1)); pcb.add(sw);
  const ledPkg = new THREE.Mesh(new THREE.BoxGeometry(0.12, 0.08, 0.1), new THREE.MeshStandardMaterial({ color: 0xf2f2ee, roughness: 0.4 }));
  ledPkg.position.copy(P(0.05, 0.03, 0.04)); pcb.add(ledPkg);

  // 8-pin JST SH (JTAG/UART), left edge, opening to the left.
  const jstMat = new THREE.MeshStandardMaterial({ color: 0xeeeae2, roughness: 0.55 });
  const jst = new THREE.Mesh(new THREE.BoxGeometry(0.43, 0.29, 1.1), jstMat);
  jst.position.copy(P(0.085, 0.335, 0.145)); jst.castShadow = true; pcb.add(jst);
  const jstSlot = new THREE.Mesh(new THREE.BoxGeometry(0.02, 0.15, 0.96), new THREE.MeshBasicMaterial({ color: 0x2a2a2a }));
  jstSlot.position.copy(P(0.085, 0.335, 0.15)); jstSlot.position.x -= 0.215; pcb.add(jstSlot);

  // Passives (instanced): input dividers arrays (two clusters), decoupling around the chips.
  const dummy = new THREE.Object3D();
  const res = [], caps = [];
  const r = rng(42);
  for (let i = 0; i < 16; i++) for (let k = 0; k < 5; k++) {   // per channel: divider, filter, series
    const cl = i < 8 ? 0 : 1;
    const u = (cl ? 0.55 : 0.15) + (i % 8) * 0.036, v = 0.57 + k * 0.034 + (cl ? -0.03 : 0);
    (k === 2 ? caps : res).push([u, v, 0]);
  }
  for (let i = 0; i < 90; i++) {   // decoupling/misc
    const near = [[0.665, 0.40, 0.28], [0.31, 0.35, 0.16], [0.47, 0.17, 0.14], [0.2, 0.12, 0.1]][i % 4];
    const a = r() * Math.PI * 2, d = near[2] * (0.75 + 0.3 * r());
    (r() < 0.55 ? caps : res).push([near[0] + Math.cos(a) * d, near[1] + Math.sin(a) * d, r() < 0.5 ? Math.PI / 2 : 0]);
  }
  const mkInst = (list, color, size) => {
    const m = new THREE.InstancedMesh(new THREE.BoxGeometry(...size), new THREE.MeshStandardMaterial({ color, roughness: 0.55 }), list.length);
    list.forEach(([u, v, ry], i) => { dummy.position.copy(P(u, v, size[1] / 2)); dummy.rotation.set(0, ry, 0); dummy.updateMatrix(); m.setMatrixAt(i, dummy.matrix); });
    pcb.add(m);
  };
  mkInst(res, 0x15161a, [0.08, 0.03, 0.04]);
  mkInst(caps, 0x9c7a52, [0.08, 0.04, 0.04]);
  anchors.dividers = P(0.47, 0.64, 0.05);

  // 2x10 right angle female header (front edge) and its gold legs back onto the pads.
  const gold = new THREE.MeshStandardMaterial({ color: 0xd9b45c, metalness: 1.0, roughness: 0.25 });
  const hb = new THREE.Mesh(new THREE.BoxGeometry(hw, 0.36, 0.42), [hdrMat, hdrMat, hdrMat, hdrMat, faceMat, hdrMat]);
  hb.position.copy(P(0.47, 0.955, 0.18)); hb.castShadow = true; pcb.add(hb);
  for (let rr = 0; rr < 2; rr++) for (let i = 0; i < 10; i++) {
    const u = 0.098 + i * 0.0828, vp = rr ? 0.792 : 0.842, yh = rr ? 0.27 : 0.14;
    const z0 = P(u, vp).z, z1 = P(u, 0.955).z - 0.21;
    const horiz = new THREE.Mesh(new THREE.BoxGeometry(0.05, 0.05, z1 - z0), gold);
    horiz.position.set(P(u, 0).x, BT + yh, (z0 + z1) / 2); horiz.castShadow = true; pcb.add(horiz);
    const vert = new THREE.Mesh(new THREE.BoxGeometry(0.05, yh, 0.05), gold);
    vert.position.set(P(u, 0).x, BT + yh / 2, z0); pcb.add(vert);
  }

  // Data path on the board (header -> dividers -> FPGA -> VL162 mux -> USB-C), board local coordinates.
  const path = new THREE.CatmullRomCurve3([P(0.47, 0.93), P(0.47, 0.70), P(0.6, 0.52), P(0.665, 0.40), P(0.45, 0.37),
    P(0.31, 0.35), P(0.38, 0.2), P(0.47, 0.08)].map(v => v.add(new THREE.Vector3(0, 0.11, 0))));

  return { root, sleeve, capF, capB, caseMats, pcb, led, ledMat, ledGlow, anchors, path };
}
