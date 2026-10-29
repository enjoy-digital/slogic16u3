// Procedural 3D model of the Sipeed SLogic16U3 (stylized): anodized aluminum case (40 x 40 x 10 mm,
// bottom tray + lid), 2x10 input header (CH0-CH15 + GND) on the front, USB-C, LED and MODE button on the
// back, and the board inside (Gowin GW5AT-15, SPI Flash, 125MHz oscillator, VL162 Type-C mux, input
// dividers). 1 world unit = 10 mm.
import * as THREE from 'three';
import { RoundedBoxGeometry } from 'three/addons/geometries/RoundedBoxGeometry.js';

export const SIZE = 4.0, HEIGHT = 1.0;

function canvas(w, h) { const c = document.createElement('canvas'); c.width = w; c.height = h; return c; }
function tex(c) { const t = new THREE.CanvasTexture(c); t.colorSpace = THREE.SRGBColorSpace; t.anisotropy = 16; return t; }

// Chip with a printed top (canvas texture).
function chip(w, h, d, lines, { color = 0x15171a, ink = '#c9ced6', font = 44, sub = 30, pins = null } = {}) {
  const g = new THREE.Group();
  const c = canvas(512, Math.round(512 * d / w));
  const x = c.getContext('2d');
  x.fillStyle = '#16181c'; x.fillRect(0, 0, c.width, c.height);
  x.fillStyle = 'rgba(255,255,255,0.06)'; x.beginPath(); x.arc(40, 40, 14, 0, 7); x.fill();
  x.fillStyle = ink; x.textAlign = 'center'; x.textBaseline = 'middle';
  lines.forEach((l, i) => {
    x.font = `${i ? 400 : 700} ${i ? sub : font}px Ubuntu Mono`;
    x.fillText(l, c.width / 2, c.height / 2 + (i - (lines.length - 1) / 2) * font * 1.15);
  });
  const top = new THREE.MeshStandardMaterial({ map: tex(c), roughness: 0.55, metalness: 0.05 });
  const side = new THREE.MeshStandardMaterial({ color, roughness: 0.6 });
  const m = new THREE.Mesh(new THREE.BoxGeometry(w, h, d), [side, side, top, side, side, side]);
  m.position.y = h / 2; m.castShadow = true; g.add(m);
  if (pins) {   // gull wing pins on the long sides
    const pm = new THREE.MeshStandardMaterial({ color: 0xb8bcc2, metalness: 0.9, roughness: 0.3 });
    for (let s of [-1, 1]) for (let i = 0; i < pins; i++) {
      const p = new THREE.Mesh(new THREE.BoxGeometry(0.035, 0.02, 0.09), pm);
      p.position.set(-w / 2 + w * (i + 0.5) / pins, 0.01, s * (d / 2 + 0.04)); g.add(p);
    }
  }
  return g;
}

function labelTexture() {
  const c = canvas(1024, 1024), x = c.getContext('2d');
  x.clearRect(0, 0, 1024, 1024);
  x.fillStyle = 'rgba(200,210,222,0.55)'; x.textAlign = 'left'; x.textBaseline = 'alphabetic';
  x.font = '700 92px Ubuntu'; x.fillText('SLogic16U3', 96, 820);
  x.fillStyle = 'rgba(92,255,138,0.75)';
  x.font = '400 46px Ubuntu Mono'; x.fillText('LITEX GATEWARE', 100, 890);
  // channel marks along the header edge
  x.fillStyle = 'rgba(200,210,222,0.45)'; x.font = '400 30px Ubuntu Mono'; x.textAlign = 'center';
  for (let i = 0; i < 10; i++) x.fillText(String(i * 2).padStart(2, '0'), 202 + i * 69, 990);
  x.strokeStyle = 'rgba(200,210,222,0.25)'; x.lineWidth = 3;
  x.strokeRect(70, 70, 884, 884);
  return tex(c);
}

export function buildDevice() {
  const root = new THREE.Group();
  const anchors = {};

  const alu = new THREE.MeshPhysicalMaterial({ color: 0x2a2e35, metalness: 0.75, roughness: 0.52, clearcoat: 0.15, clearcoatRoughness: 0.6 });
  const aluDark = new THREE.MeshStandardMaterial({ color: 0x14171b, metalness: 0.7, roughness: 0.5 });

  // Case: bottom tray + lid.
  const tray = new THREE.Mesh(new RoundedBoxGeometry(SIZE, 0.55, SIZE, 6, 0.1), alu);
  tray.position.y = 0.275; tray.castShadow = tray.receiveShadow = true; root.add(tray);
  const lid = new THREE.Group(); root.add(lid);
  const lidMat = alu.clone(); lidMat.transparent = true;
  const lidMesh = new THREE.Mesh(new RoundedBoxGeometry(SIZE, 0.45, SIZE, 6, 0.1), lidMat);
  lidMesh.position.y = 0.775; lidMesh.castShadow = true; lid.add(lidMesh);
  const labelMat = new THREE.MeshBasicMaterial({ map: labelTexture(), transparent: true, depthWrite: false });
  const label = new THREE.Mesh(new THREE.PlaneGeometry(SIZE * 0.98, SIZE * 0.98), labelMat);
  label.rotation.x = -Math.PI / 2; label.position.y = HEIGHT + 0.002; lid.add(label);
  // Seam between tray and lid.
  const seam = new THREE.Mesh(new THREE.BoxGeometry(SIZE * 1.002, 0.012, SIZE * 1.002), aluDark);
  seam.position.y = 0.552; root.add(seam);

  // Front: 2x10 header (black housing + gold pins).
  const hdr = new THREE.Group(); root.add(hdr);
  const hdrMat = new THREE.MeshStandardMaterial({ color: 0x0b0c0e, roughness: 0.7 });
  const housing = new THREE.Mesh(new THREE.BoxGeometry(2.6, 0.5, 0.26), hdrMat);
  housing.position.set(0, 0.52, SIZE / 2 + 0.06); hdr.add(housing);
  const gold = new THREE.MeshStandardMaterial({ color: 0xd8b25a, metalness: 1.0, roughness: 0.25 });
  for (let r = 0; r < 2; r++) for (let i = 0; i < 10; i++) {
    const p = new THREE.Mesh(new THREE.BoxGeometry(0.064, 0.064, 0.36), gold);
    p.position.set(-1.143 + i * 0.254, 0.52 + (r ? -0.127 : 0.127), SIZE / 2 + 0.25);
    p.castShadow = true; hdr.add(p);
  }
  anchors.header = new THREE.Vector3(-0.9, 0.7, SIZE / 2 + 0.3);

  // Back: USB-C (stadium shell), LED, MODE button.
  const shellMat = new THREE.MeshStandardMaterial({ color: 0xc9ccd2, metalness: 1.0, roughness: 0.22 });
  const shape = new THREE.Shape(); const uw = 0.9, uh = 0.32, ur = uh / 2;
  shape.absarc(-uw / 2 + ur, 0, ur, Math.PI / 2, 3 * Math.PI / 2); shape.absarc(uw / 2 - ur, 0, ur, -Math.PI / 2, Math.PI / 2);
  const hole = new THREE.Path(); const iw = uw - 0.06, ir = (uh - 0.06) / 2;
  hole.absarc(-iw / 2 + ir, 0, ir, Math.PI / 2, 3 * Math.PI / 2); hole.absarc(iw / 2 - ir, 0, ir, -Math.PI / 2, Math.PI / 2);
  shape.holes.push(hole);
  const shell = new THREE.Mesh(new THREE.ExtrudeGeometry(shape, { depth: 0.08, bevelEnabled: false }), shellMat);
  shell.position.set(0.4, 0.5, -SIZE / 2 - 0.06); shell.rotation.y = Math.PI; root.add(shell);
  const cavity = new THREE.Mesh(new THREE.BoxGeometry(uw - 0.08, uh - 0.08, 0.05), new THREE.MeshBasicMaterial({ color: 0x050506 }));
  cavity.position.set(0.4, 0.5, -SIZE / 2 - 0.01); root.add(cavity);
  const tongue = new THREE.Mesh(new THREE.BoxGeometry(0.5, 0.06, 0.06), new THREE.MeshStandardMaterial({ color: 0x1a1c20 }));
  tongue.position.set(0.4, 0.5, -SIZE / 2 - 0.03); root.add(tongue);
  anchors.usb = new THREE.Vector3(0.4, 0.5, -SIZE / 2 - 0.1);
  const ledMat = new THREE.MeshBasicMaterial({ color: 0xff3838 });
  const led = new THREE.Mesh(new THREE.CylinderGeometry(0.05, 0.05, 0.03, 20), ledMat);
  led.rotation.x = Math.PI / 2; led.position.set(-0.55, 0.5, -SIZE / 2 - 0.012); root.add(led);
  const ledGlow = new THREE.Mesh(new THREE.CircleGeometry(0.16, 24), new THREE.MeshBasicMaterial({ color: 0xff3838, transparent: true, opacity: 0.35, depthWrite: false }));
  ledGlow.rotation.y = Math.PI; ledGlow.position.set(-0.55, 0.5, -SIZE / 2 - 0.03); root.add(ledGlow);
  anchors.led = new THREE.Vector3(-0.55, 0.5, -SIZE / 2 - 0.05);
  const btn = new THREE.Mesh(new THREE.CylinderGeometry(0.09, 0.09, 0.06, 24), aluDark);
  btn.rotation.x = Math.PI / 2; btn.position.set(-1.2, 0.5, -SIZE / 2 - 0.02); root.add(btn);
  anchors.mode = new THREE.Vector3(-1.2, 0.5, -SIZE / 2 - 0.05);

  // Board (inside).
  const pcb = new THREE.Group(); pcb.position.y = 0.42; root.add(pcb);
  const c = canvas(1024, 1024), x = c.getContext('2d');
  x.fillStyle = '#0d1f17'; x.fillRect(0, 0, 1024, 1024);
  x.strokeStyle = 'rgba(214,180,96,0.22)'; x.lineWidth = 3;
  for (let i = 0; i < 16; i++) {   // input traces: header -> dividers -> FPGA
    const hx = 200 + i * 39;
    x.beginPath(); x.moveTo(hx, 1024); x.lineTo(hx, 860); x.lineTo(420 + i * 12, 640); x.lineTo(420 + i * 12, 560); x.stroke();
  }
  x.lineWidth = 6; x.strokeStyle = 'rgba(214,180,96,0.32)';   // SerDes pairs FPGA -> mux -> USB
  for (const dx of [-14, 14]) { x.beginPath(); x.moveTo(512 + dx, 400); x.lineTo(612 + dx, 260); x.lineTo(612 + dx, 120); x.stroke(); }
  x.fillStyle = 'rgba(255,255,255,0.35)'; x.font = '400 22px Ubuntu Mono';
  x.fillText('SLogic16U3', 60, 80);
  const pcbTop = new THREE.MeshStandardMaterial({ map: tex(c), roughness: 0.5, metalness: 0.1 });
  const pcbSide = new THREE.MeshStandardMaterial({ color: 0x0b1912, roughness: 0.6 });
  const board = new THREE.Mesh(new THREE.BoxGeometry(3.7, 0.08, 3.7), [pcbSide, pcbSide, pcbTop, pcbSide, pcbSide, pcbSide]);
  board.position.y = 0.04; board.receiveShadow = true; pcb.add(board);
  const T = 0.08;
  // map board canvas px -> local
  const P = (px, pz) => new THREE.Vector3((px / 1024 - 0.5) * 3.7, T, (pz / 1024 - 0.5) * 3.7);

  const fpga = chip(0.8, 0.09, 0.8, ['GOWIN', 'GW5AT-15', 'MBGA132'], { font: 70, sub: 56 });
  fpga.position.copy(P(470, 470)); pcb.add(fpga);
  anchors.fpga = P(470, 470).add(new THREE.Vector3(0, 0.1, 0));
  const flash = chip(0.5, 0.06, 0.38, ['SPI FLASH', '16MB'], { font: 64, sub: 56, pins: 4 });
  flash.position.copy(P(230, 330)); pcb.add(flash);
  anchors.flash = P(230, 330).add(new THREE.Vector3(0, 0.07, 0));
  const osc = chip(0.32, 0.09, 0.25, ['125.000'], { font: 90, color: 0x9ca1a8 });
  osc.position.copy(P(760, 420)); pcb.add(osc);
  anchors.osc = P(760, 420).add(new THREE.Vector3(0, 0.1, 0));
  const mux = chip(0.4, 0.05, 0.4, ['VL162'], { font: 110 });
  mux.position.copy(P(612, 240)); pcb.add(mux);
  anchors.mux = P(612, 240).add(new THREE.Vector3(0, 0.06, 0));
  const rmat = new THREE.MeshStandardMaterial({ color: 0x202226, roughness: 0.6 });
  const cap = new THREE.MeshStandardMaterial({ color: 0xb08a56, roughness: 0.5 });
  for (let i = 0; i < 16; i++) {   // input dividers
    const r = new THREE.Mesh(new THREE.BoxGeometry(0.06, 0.03, 0.12), i % 2 ? cap : rmat);
    r.position.copy(P(200 + i * 39, 820)); r.position.y += 0.015; pcb.add(r);
  }
  anchors.dividers = P(512, 820).add(new THREE.Vector3(0, 0.05, 0));
  const rec = new THREE.Mesh(new THREE.BoxGeometry(0.95, 0.34, 0.5), shellMat);
  rec.position.copy(P(612, 40)); rec.position.y += 0.17; pcb.add(rec);
  const hdrPcb = new THREE.Mesh(new THREE.BoxGeometry(2.6, 0.12, 0.3), hdrMat);
  hdrPcb.position.copy(P(512, 1010)); hdrPcb.position.y += 0.06; pcb.add(hdrPcb);

  // Data path on the board (header -> dividers -> FPGA -> mux -> USB-C), board local coordinates.
  const path = new THREE.CatmullRomCurve3([P(512, 1000), P(512, 830), P(500, 640), P(470, 470), P(600, 300), P(612, 240), P(612, 60)]
    .map(v => v.add(new THREE.Vector3(0, 0.11, 0))));

  return { root, lid, lidMat, labelMat, pcb, hdr, led, ledMat, ledGlow, anchors, path };
}
