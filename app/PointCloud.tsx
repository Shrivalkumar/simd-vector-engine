"use client";

import { useEffect, useRef } from "react";
import * as THREE from "three";

function seeded(index: number) {
  const value = Math.sin(index * 12.9898) * 43758.5453;
  return value - Math.floor(value);
}

export function PointCloud({ activeCluster, pointCount = 2400 }: { activeCluster: number; pointCount?: number }) {
  const ref = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const canvas = ref.current;
    if (!canvas) return;
    const scene = new THREE.Scene();
    const camera = new THREE.PerspectiveCamera(43, 1, 0.1, 100);
    camera.position.set(0, 0.3, 9.3);
    const renderer = new THREE.WebGLRenderer({ canvas, antialias: true, alpha: true });
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    const root = new THREE.Group();
    scene.add(root);
    const geometry = new THREE.BufferGeometry();
    const count = Math.min(2400, Math.max(240, pointCount));
    const positions = new Float32Array(count * 3);
    const colors = new Float32Array(count * 3);
    const cyan = new THREE.Color("#4bd7ff");
    const blue = new THREE.Color("#3b78ff");
    const amber = new THREE.Color("#ffbd58");
    const color = new THREE.Color();
    for (let index = 0; index < count; index += 1) {
      const cluster = index % 3;
      const radius = Math.pow(seeded(index + 7), 0.56) * 1.4;
      const theta = seeded(index + 11) * Math.PI * 2;
      const phi = Math.acos(2 * seeded(index + 23) - 1);
      const offset = cluster === 0 ? -2.1 : cluster === 1 ? 0.15 : 2.2;
      positions[index * 3] = offset + radius * Math.sin(phi) * Math.cos(theta);
      positions[index * 3 + 1] = radius * Math.cos(phi) * 0.86;
      positions[index * 3 + 2] = radius * Math.sin(phi) * Math.sin(theta) * 0.66;
      color.copy(cluster === 2 ? amber : cluster === 1 ? blue : cyan);
      color.multiplyScalar(0.54 + seeded(index + 41) * 0.52);
      colors[index * 3] = color.r;
      colors[index * 3 + 1] = color.g;
      colors[index * 3 + 2] = color.b;
    }
    geometry.setAttribute("position", new THREE.BufferAttribute(positions, 3));
    geometry.setAttribute("color", new THREE.BufferAttribute(colors, 3));
    const material = new THREE.PointsMaterial({ size: 0.043, vertexColors: true, transparent: true, opacity: 0.88, sizeAttenuation: true, depthWrite: false });
    root.add(new THREE.Points(geometry, material));
    const rings = [cyan, blue, amber].map((ringColor, index) => {
      const ring = new THREE.Mesh(new THREE.TorusGeometry(1.34, 0.008, 6, 84), new THREE.MeshBasicMaterial({ color: ringColor, transparent: true, opacity: 0.36 }));
      ring.position.x = index === 0 ? -2.1 : index === 1 ? 0.15 : 2.2;
      ring.rotation.x = Math.PI / 2.7;
      root.add(ring);
      return ring;
    });
    let pointerX = 0;
    let pointerY = 0;
    const onPointer = (event: PointerEvent) => {
      const bounds = canvas.getBoundingClientRect();
      pointerX = (event.clientX - bounds.left) / bounds.width - 0.5;
      pointerY = (event.clientY - bounds.top) / bounds.height - 0.5;
    };
    canvas.addEventListener("pointermove", onPointer);
    const resize = () => {
      const { width, height } = canvas.getBoundingClientRect();
      renderer.setSize(width, height, false);
      camera.aspect = width / height;
      camera.updateProjectionMatrix();
    };
    const observer = new ResizeObserver(resize);
    observer.observe(canvas);
    resize();
    let frame = 0;
    const render = () => {
      root.rotation.y += 0.0023;
      root.rotation.x += (pointerY * 0.17 - root.rotation.x) * 0.025;
      root.rotation.y += pointerX * 0.0008;
      rings.forEach((ring, index) => {
        ring.scale.setScalar(index === activeCluster ? 1.18 + Math.sin(performance.now() / 230) * 0.025 : 1);
        (ring.material as THREE.MeshBasicMaterial).opacity = index === activeCluster ? 0.92 : 0.25;
      });
      renderer.render(scene, camera);
      frame = requestAnimationFrame(render);
    };
    render();
    return () => {
      cancelAnimationFrame(frame);
      observer.disconnect();
      canvas.removeEventListener("pointermove", onPointer);
      geometry.dispose();
      material.dispose();
      rings.forEach((ring) => { ring.geometry.dispose(); (ring.material as THREE.Material).dispose(); });
      renderer.dispose();
    };
  }, [activeCluster, pointCount]);

  return <canvas ref={ref} aria-label="Three-dimensional sampled vector projection" />;
}
