/**
 * @file login-stellar.js
 * 用途与使用：登录页的 Three.js 背景动画；仅处理显示与事件清理，不处理鉴权口令。
 * 调用约束：页面切换时遵循既有轮询/事件生命周期；不可把浏览器缓存当设备当前状态。
 * 维护：外层目录可改名；模块与接口名保持稳定，配置经服务端校验。
 */
// Decorative scene only. Authentication stays with the existing POST /login form.
try {
const THREE = await import('/static/vendor/three.module.min.js');
    const canvas = document.getElementById('webgl');
    const renderer = new THREE.WebGLRenderer({
      canvas,
      antialias: true,
      alpha: false
    });
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    renderer.setSize(window.innerWidth, window.innerHeight);
    renderer.setClearColor(0x02030a, 1);

    const particleCanvas = document.createElement('canvas');
    particleCanvas.width = particleCanvas.height = 64;
    const particleContext = particleCanvas.getContext('2d');
    const particleGradient = particleContext.createRadialGradient(32,32,0,32,32,30);
    particleGradient.addColorStop(0,'rgba(255,255,255,1)');
    particleGradient.addColorStop(.24,'rgba(255,255,255,.9)');
    particleGradient.addColorStop(.65,'rgba(255,255,255,.22)');
    particleGradient.addColorStop(1,'rgba(255,255,255,0)');
    particleContext.fillStyle=particleGradient;particleContext.fillRect(0,0,64,64);
    const particleTexture = new THREE.CanvasTexture(particleCanvas);
    const scene = new THREE.Scene();
    scene.fog = new THREE.FogExp2(0x02030a, 0.0022);

    const camera = new THREE.PerspectiveCamera(
      52,
      window.innerWidth / window.innerHeight,
      0.1,
      2000
    );
    // 相机稍微抬高, 往下看星系, 同时给底部面板留空间
    camera.position.set(0, 30, 66);
    camera.lookAt(0, 0, 0);

    /* ============================================================
       光照
    ============================================================ */
    const starLight = new THREE.PointLight(0xffb347, 3.2, 240, 2);
    starLight.position.set(0, 0, 0);
    scene.add(starLight);

    const ambient = new THREE.AmbientLight(0x223055, 0.9);
    scene.add(ambient);

    const rimLight = new THREE.DirectionalLight(0x4dd0e1, 0.6);
    rimLight.position.set(-40, 30, -30);
    scene.add(rimLight);

    /* ============================================================
       星空 : 三层粒子
    ============================================================ */
    function createStarLayer(count, radius, size, color, opacity) {
      const geo = new THREE.BufferGeometry();
      const positions = new Float32Array(count * 3);
      const colors = new Float32Array(count * 3);
      const baseColor = new THREE.Color(color);

      for (let i = 0; i < count; i++) {
        const r = radius * (0.7 + Math.random() * 0.3);
        const theta = Math.random() * Math.PI * 2;
        const phi = Math.acos(2 * Math.random() - 1);

        positions[i * 3]     = r * Math.sin(phi) * Math.cos(theta);
        positions[i * 3 + 1] = r * Math.sin(phi) * Math.sin(theta);
        positions[i * 3 + 2] = r * Math.cos(phi);

        const c = baseColor.clone();
        c.offsetHSL(0, 0, (Math.random() - 0.5) * 0.3);
        colors[i * 3]     = c.r;
        colors[i * 3 + 1] = c.g;
        colors[i * 3 + 2] = c.b;
      }

      geo.setAttribute('position', new THREE.BufferAttribute(positions, 3));
      geo.setAttribute('color', new THREE.BufferAttribute(colors, 3));

      const mat = new THREE.PointsMaterial({
      map: particleTexture, alphaTest: .01,
        size: size,
        vertexColors: true,
        transparent: true,
        opacity: opacity,
        depthWrite: false,
        blending: THREE.AdditiveBlending,
        sizeAttenuation: true
      });

      return new THREE.Points(geo, mat);
    }

    const starsFar  = createStarLayer(1100, 620, 1.1, 0x9fb8ff, 0.75);
    const starsMid  = createStarLayer(700, 420, 1.6, 0xcfe4ff, 0.85);
    const starsNear = createStarLayer(350,  240, 2.4, 0xffffff, 0.95);

    scene.add(starsFar);
    scene.add(starsMid);
    scene.add(starsNear);

    /* ============================================================
       恒星 : 核心 + 光晕 + 日冕
    ============================================================ */
    const starGroup = new THREE.Group();
    scene.add(starGroup);

    const coreUniforms = {
      uTime: { value: 0 },
      uColorA: { value: new THREE.Color(0xfff3c4) },
      uColorB: { value: new THREE.Color(0xffb020) },
      uColorC: { value: new THREE.Color(0xff4e00) }
    };

    const coreMat = new THREE.ShaderMaterial({
      uniforms: coreUniforms,
      vertexShader: `
        varying vec3 vNormal;
        varying vec2 vUv;
        varying vec3 vPos;

        void main() {
          vNormal = normalize(normalMatrix * normal);
          vUv = uv;
          vPos = position;
          gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
        }
      `,
      fragmentShader: `
        uniform float uTime;
        uniform vec3 uColorA;
        uniform vec3 uColorB;
        uniform vec3 uColorC;

        varying vec3 vNormal;
        varying vec2 vUv;
        varying vec3 vPos;

        float hash(vec3 p) {
          return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453123);
        }

        float noise(vec3 p) {
          vec3 i = floor(p);
          vec3 f = fract(p);
          f = f * f * (3.0 - 2.0 * f);
          float n = mix(
            mix(mix(hash(i + vec3(0,0,0)), hash(i + vec3(1,0,0)), f.x),
                mix(hash(i + vec3(0,1,0)), hash(i + vec3(1,1,0)), f.x), f.y),
            mix(mix(hash(i + vec3(0,0,1)), hash(i + vec3(1,0,1)), f.x),
                mix(hash(i + vec3(0,1,1)), hash(i + vec3(1,1,1)), f.x), f.y),
            f.z
          );
          return n;
        }

        void main() {
          vec3 p = vPos * 0.06;
          float n1 = noise(p + vec3(uTime * 0.15, 0.0, 0.0));
          float n2 = noise(p * 2.1 + vec3(0.0, uTime * 0.22, 0.0));
          float n3 = noise(p * 4.3 + vec3(0.0, 0.0, uTime * 0.18));

          float n = n1 * 0.55 + n2 * 0.3 + n3 * 0.15;

          vec3 color = mix(uColorC, uColorB, smoothstep(0.25, 0.6, n));
          color = mix(color, uColorA, smoothstep(0.6, 0.95, n));

          float fresnel = pow(1.0 - abs(dot(vNormal, vec3(0.0, 0.0, 1.0))), 2.2);
          color += vec3(1.0, 0.8, 0.4) * fresnel * 0.9;

          float flare = pow(max(n - 0.75, 0.0) / 0.25, 2.0);
          color += vec3(1.0, 0.95, 0.8) * flare * 0.8;

          gl_FragColor = vec4(color, 1.0);
        }
      `
    });

    const starCore = new THREE.Mesh(
      new THREE.SphereGeometry(6, 64, 64),
      coreMat
    );
    starGroup.add(starCore);

    function makeGlowTexture() {
      const size = 256;
      const c = document.createElement('canvas');
      c.width = c.height = size;
      const ctx = c.getContext('2d');
      const g = ctx.createRadialGradient(
        size / 2, size / 2, 0,
        size / 2, size / 2, size / 2
      );
      g.addColorStop(0, 'rgba(255,255,255,1)');
      g.addColorStop(0.25, 'rgba(255,220,150,0.85)');
      g.addColorStop(0.55, 'rgba(255,150,60,0.35)');
      g.addColorStop(1, 'rgba(255,100,20,0)');
      ctx.fillStyle = g;
      ctx.fillRect(0, 0, size, size);
      const tex = new THREE.CanvasTexture(c);
      tex.needsUpdate = true;
      return tex;
    }

    const glowMat1 = new THREE.SpriteMaterial({
      map: makeGlowTexture(),
      color: 0xffb347,
      transparent: true,
      opacity: 0.9,
      blending: THREE.AdditiveBlending,
      depthWrite: false
    });
    const glow1 = new THREE.Sprite(glowMat1);
    glow1.scale.set(36, 36, 1);
    starGroup.add(glow1);

    const glowMat2 = new THREE.SpriteMaterial({
      map: makeGlowTexture(),
      color: 0xff7a1a,
      transparent: true,
      opacity: 0.55,
      blending: THREE.AdditiveBlending,
      depthWrite: false
    });
    const glow2 = new THREE.Sprite(glowMat2);
    glow2.scale.set(80, 80, 1);
    starGroup.add(glow2);

    /* 日冕粒子 */
    const coronaCount = 420;
    const coronaGeo = new THREE.BufferGeometry();
    const coronaPos = new Float32Array(coronaCount * 3);
    const coronaSeed = new Float32Array(coronaCount);

    for (let i = 0; i < coronaCount; i++) {
      const r = 7 + Math.random() * 5;
      const theta = Math.random() * Math.PI * 2;
      const phi = Math.acos(2 * Math.random() - 1);

      coronaPos[i * 3]     = r * Math.sin(phi) * Math.cos(theta);
      coronaPos[i * 3 + 1] = r * Math.sin(phi) * Math.sin(theta) * 0.6;
      coronaPos[i * 3 + 2] = r * Math.cos(phi);

      coronaSeed[i] = Math.random() * Math.PI * 2;
    }
    coronaGeo.setAttribute('position', new THREE.BufferAttribute(coronaPos, 3));
    coronaGeo.setAttribute('aSeed', new THREE.BufferAttribute(coronaSeed, 1));

    const coronaMat = new THREE.PointsMaterial({
      map: particleTexture, alphaTest: .01,
      size: 0.9,
      color: 0xffcf80,
      transparent: true,
      opacity: 0.7,
      blending: THREE.AdditiveBlending,
      depthWrite: false,
      sizeAttenuation: true
    });

    const corona = new THREE.Points(coronaGeo, coronaMat);
    starGroup.add(corona);

    /* ============================================================
       行星系统
    ============================================================ */
    const planets = [];

    function createOrbit(radius, color, opacity) {
      const segments = 256;
      const geo = new THREE.BufferGeometry();
      const pts = [];
      for (let i = 0; i <= segments; i++) {
        const a = (i / segments) * Math.PI * 2;
        pts.push(new THREE.Vector3(Math.cos(a) * radius, 0, Math.sin(a) * radius));
      }
      geo.setFromPoints(pts);

      const mat = new THREE.LineBasicMaterial({
        color: color,
        transparent: true,
        opacity: opacity,
        blending: THREE.AdditiveBlending,
        depthWrite: false
      });
      return new THREE.Line(geo, mat);
    }

    function createPlanet(opts) {
      const {
        radius,
        orbitRadius,
        orbitSpeed,
        color,
        emissive,
        inclination,
        hasRing
      } = opts;

      const group = new THREE.Group();
      group.rotation.x = inclination;

      const mat = new THREE.MeshStandardMaterial({
        color: color,
        emissive: emissive,
        emissiveIntensity: 0.35,
        roughness: 0.7,
        metalness: 0.25
      });

      const mesh = new THREE.Mesh(
        new THREE.SphereGeometry(radius, 32, 32),
        mat
      );
      group.add(mesh);

      const haloMat = new THREE.SpriteMaterial({
        map: makeGlowTexture(),
        color: color,
        transparent: true,
        opacity: 0.55,
        blending: THREE.AdditiveBlending,
        depthWrite: false
      });
      const halo = new THREE.Sprite(haloMat);
      halo.scale.set(radius * 6, radius * 6, 1);
      group.add(halo);

      if (hasRing) {
        const ringGeo = new THREE.RingGeometry(radius * 1.5, radius * 2.4, 64);
        const ringMat = new THREE.MeshBasicMaterial({
          color: color,
          transparent: true,
          opacity: 0.35,
          side: THREE.DoubleSide,
          blending: THREE.AdditiveBlending,
          depthWrite: false
        });
        const ring = new THREE.Mesh(ringGeo, ringMat);
        ring.rotation.x = Math.PI / 2.2;
        group.add(ring);
      }

      mesh.position.set(orbitRadius, 0, 0);
      halo.position.copy(mesh.position);

      const orbitLine = createOrbit(orbitRadius, color, 0.22);
      orbitLine.rotation.x = inclination;

      scene.add(orbitLine);
      scene.add(group);

      return {
        group,
        mesh,
        halo,
        orbitRadius,
        orbitSpeed,
        angle: Math.random() * Math.PI * 2,
        inclination
      };
    }

    planets.push(createPlanet({
      radius: 1.1,
      orbitRadius: 14,
      orbitSpeed: 0.55,
      color: 0x818cf8,
      emissive: 0x4338ca,
      inclination: 0.15,
      hasRing: false
    }));

    planets.push(createPlanet({
      radius: 0.9,
      orbitRadius: 21,
      orbitSpeed: 0.36,
      color: 0x22d3ee,
      emissive: 0x0e7490,
      inclination: -0.35,
      hasRing: false
    }));

    planets.push(createPlanet({
      radius: 1.8,
      orbitRadius: 30,
      orbitSpeed: 0.22,
      color: 0xf472b6,
      emissive: 0x9d174d,
      inclination: 0.5,
      hasRing: true
    }));

    /* ============================================================
       鼠标交互
    ============================================================ */
    const camState = {
      theta: 0,
      phi: 0.42,
      radius: 68,
      targetTheta: 0,
      targetPhi: 0.42,
      targetRadius: 68,
      dragging: false,
      lastX: 0,
      lastY: 0,
      autoRotate: true,
      resumeTimer: null
    };

    function updateCamera() {
      camState.theta += (camState.targetTheta - camState.theta) * 0.08;
      camState.phi   += (camState.targetPhi   - camState.phi)   * 0.08;
      camState.radius += (camState.targetRadius - camState.radius) * 0.08;

      const x = camState.radius * Math.sin(camState.phi) * Math.sin(camState.theta);
      const y = camState.radius * Math.cos(camState.phi);
      const z = camState.radius * Math.sin(camState.phi) * Math.cos(camState.theta);

      camera.position.set(x, y, z);
      // 视线中心稍微上移, 让星系偏向屏幕中上部 (底部面板不遮挡)
      camera.lookAt(0, -1, 0);
    }

    canvas.addEventListener('mousedown', (e) => {
      camState.dragging = true;
      camState.autoRotate = false;
      camState.lastX = e.clientX;
      camState.lastY = e.clientY;
    });

    window.addEventListener('mousemove', (e) => {
      if (!camState.dragging) return;
      const dx = e.clientX - camState.lastX;
      const dy = e.clientY - camState.lastY;
      camState.lastX = e.clientX;
      camState.lastY = e.clientY;

      camState.targetTheta -= dx * 0.005;
      camState.targetPhi   -= dy * 0.005;
      camState.targetPhi = Math.max(0.12, Math.min(Math.PI - 0.12, camState.targetPhi));
    });

    window.addEventListener('mouseup', () => {
      camState.dragging = false;
      clearTimeout(camState.resumeTimer);
      camState.resumeTimer = setTimeout(() => {
        camState.autoRotate = true;
      }, 2500);
    });

    canvas.addEventListener('wheel', (e) => {
      e.preventDefault();
      camState.targetRadius += e.deltaY * 0.05;
      camState.targetRadius = Math.max(30, Math.min(150, camState.targetRadius));
    }, { passive: false });

    canvas.addEventListener('touchstart', (e) => {
      if (e.touches.length === 1) {
        camState.dragging = true;
        camState.autoRotate = false;
        camState.lastX = e.touches[0].clientX;
        camState.lastY = e.touches[0].clientY;
      }
    }, { passive: true });

    canvas.addEventListener('touchmove', (e) => {
      if (!camState.dragging || e.touches.length !== 1) return;
      const dx = e.touches[0].clientX - camState.lastX;
      const dy = e.touches[0].clientY - camState.lastY;
      camState.lastX = e.touches[0].clientX;
      camState.lastY = e.touches[0].clientY;
      camState.targetTheta -= dx * 0.005;
      camState.targetPhi   -= dy * 0.005;
      camState.targetPhi = Math.max(0.12, Math.min(Math.PI - 0.12, camState.targetPhi));
    }, { passive: true });

    canvas.addEventListener('touchend', () => {
      camState.dragging = false;
      clearTimeout(camState.resumeTimer);
      camState.resumeTimer = setTimeout(() => {
        camState.autoRotate = true;
      }, 2500);
    }, { passive: true });

    /* ============================================================
       跃迁效果
    ============================================================ */
    let warpActive = false;
    let warpProgress = 0;

    function triggerWarp() {
      warpActive = true;
      warpProgress = 0;
    }

    /* ============================================================
       渲染循环
    ============================================================ */
    const clock = new THREE.Clock();

    function animate() {
      requestAnimationFrame(animate);
      if(document.hidden) return;

      const dt = Math.min(clock.getDelta(), 0.05);
      const t = clock.getElapsedTime();

      coreUniforms.uTime.value = t;
      starCore.rotation.y += dt * 0.08;

      corona.rotation.y += dt * 0.15;
      corona.rotation.x = Math.sin(t * 0.3) * 0.1;
      coronaMat.opacity = 0.55 + Math.sin(t * 1.8) * 0.2;

      const glowScale1 = 36 + Math.sin(t * 1.5) * 3;
      const glowScale2 = 80 + Math.sin(t * 1.2 + 1) * 6;
      glow1.scale.set(glowScale1, glowScale1, 1);
      glow2.scale.set(glowScale2, glowScale2, 1);

      planets.forEach((p) => {
        p.angle += dt * p.orbitSpeed * 0.35;
        const x = Math.cos(p.angle) * p.orbitRadius;
        const z = Math.sin(p.angle) * p.orbitRadius;
        p.mesh.position.set(x, 0, z);
        p.halo.position.set(x, 0, z);
        p.mesh.rotation.y += dt * 0.6;
      });

      starsFar.rotation.y  += dt * 0.004;
      starsMid.rotation.y  += dt * 0.007;
      starsNear.rotation.y += dt * 0.012;
      starsNear.rotation.x += dt * 0.003;

      if (camState.autoRotate && !camState.dragging) {
        camState.targetTheta += dt * 0.06;
      }

      if (warpActive) {
        warpProgress += dt * 1.2;
        if (warpProgress < 1) {
          const ease = warpProgress * warpProgress * (3 - 2 * warpProgress);
          camState.targetRadius = 68 - ease * 46;
          camState.targetPhi = 0.42 + ease * 0.25;
          starLight.intensity = 3.2 + ease * 4;
        } else if (warpProgress < 1.8) {
          starLight.intensity = 7.2;
        } else {
          warpActive = false;
          camState.targetRadius = 68;
          camState.targetPhi = 0.42;
          starLight.intensity = 3.2;
        }
      }

      updateCamera();
      renderer.render(scene, camera);
    }

    animate();

    window.addEventListener('resize', () => {
      camera.aspect = window.innerWidth / window.innerHeight;
      camera.updateProjectionMatrix();
      renderer.setSize(window.innerWidth, window.innerHeight);
    });


document.getElementById('scene-state').textContent='3D 恒星场景 · 拖动旋转 / 滚轮缩放';
} catch (e) {
document.getElementById('scene-state').textContent='静态星空模式 · 登录正常可用';
}
