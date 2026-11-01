// Login backdrop: local Three.js black hole. Authentication remains the POST /login form.
try {
  const THREE = await import('/static/vendor/three.module.min.js');
  const canvas = document.getElementById('webgl');
  const renderer = new THREE.WebGLRenderer({canvas, antialias:true, alpha:false});
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
  renderer.setSize(window.innerWidth, window.innerHeight);
  renderer.setClearColor(0x010208, 1);
  const scene = new THREE.Scene(); scene.fog = new THREE.FogExp2(0x010208, .0018);
  const camera = new THREE.PerspectiveCamera(46, innerWidth/innerHeight, .1, 1800);
  const target = new THREE.Vector3(0, 2.5, 0);
  function roundTexture(size=64) {
    const c=document.createElement('canvas'); c.width=c.height=size;
    const x=c.getContext('2d'), g=x.createRadialGradient(size/2,size/2,0,size/2,size/2,size/2);
    g.addColorStop(0,'rgba(255,255,255,1)'); g.addColorStop(.2,'rgba(255,255,255,.95)'); g.addColorStop(.62,'rgba(255,255,255,.25)'); g.addColorStop(1,'rgba(255,255,255,0)');
    x.fillStyle=g; x.fillRect(0,0,size,size); return new THREE.CanvasTexture(c);
  }
  const particleMap=roundTexture();
  function stars(count,radius,size,opacity) {
    const p=new Float32Array(count*3), c=new Float32Array(count*3);
    for(let i=0;i<count;i++){const r=radius*(.55+Math.random()*.45),a=Math.random()*Math.PI*2,z=(Math.random()-.5)*radius*1.4,rr=Math.sqrt(Math.max(0,r*r-z*z));p.set([Math.cos(a)*rr,z+4,Math.sin(a)*rr-8],i*3);const q=new THREE.Color(Math.random()<.72?0x9cc7ff:0xffc886);q.offsetHSL(0,0,(Math.random()-.5)*.22);c.set([q.r,q.g,q.b],i*3);}
    const g=new THREE.BufferGeometry();g.setAttribute('position',new THREE.BufferAttribute(p,3));g.setAttribute('color',new THREE.BufferAttribute(c,3));
    return new THREE.Points(g,new THREE.PointsMaterial({map:particleMap,size,vertexColors:true,transparent:true,opacity,depthWrite:false,blending:THREE.AdditiveBlending,sizeAttenuation:true,alphaTest:.01}));
  }
  const starsFar=stars(1500,180,1.1,.8);scene.add(starsFar);
  const hole=new THREE.Group();hole.position.y=2.2;scene.add(hole);
  hole.add(new THREE.Mesh(new THREE.SphereGeometry(5.9,64,64),new THREE.MeshBasicMaterial({color:0x000105})));
  function ring(radius,tube,color,opacity){const m=new THREE.Mesh(new THREE.TorusGeometry(radius,tube,18,180),new THREE.MeshBasicMaterial({color,transparent:true,opacity,blending:THREE.AdditiveBlending,depthWrite:false}));m.rotation.x=Math.PI/2;hole.add(m);return m;}
  const photon=ring(6.75,.17,0xffd9a0,.98), lens=ring(7.35,.34,0xff741e,.42), outer=ring(10.2,.16,0x7bd8ff,.32);
  const disk=new THREE.Group();disk.rotation.x=-.16;disk.rotation.z=.08;hole.add(disk);
  for(const [r,t,c,o] of [[8.2,1,0xffc36b,.2],[10.7,1.1,0xff7a20,.16],[14.2,.85,0x49b9e8,.12],[18,.5,0x2468a7,.08]]){const m=new THREE.Mesh(new THREE.TorusGeometry(r,t,14,220),new THREE.MeshBasicMaterial({color:c,transparent:true,opacity:o,side:THREE.DoubleSide,blending:THREE.AdditiveBlending,depthWrite:false}));m.rotation.x=Math.PI/2;disk.add(m);}
  const n=1700,pos=new Float32Array(n*3),col=new Float32Array(n*3),rad=new Float32Array(n),ang=new Float32Array(n),spd=new Float32Array(n);
  for(let i=0;i<n;i++){const r=7.4+Math.pow(Math.random(),.62)*15,a=Math.random()*Math.PI*2;rad[i]=r;ang[i]=a;spd[i]=(.45+Math.random()*.7)*(18/r);pos.set([Math.cos(a)*r,(Math.random()-.5)*(.12+1.3*r/22),Math.sin(a)*r],i*3);const q=new THREE.Color(r<10?0xffcf83:r<15?0xff7032:0x4fb7e5);q.offsetHSL(0,0,(Math.random()-.5)*.18);col.set([q.r,q.g,q.b],i*3);}
  const dustGeo=new THREE.BufferGeometry();dustGeo.setAttribute('position',new THREE.BufferAttribute(pos,3));dustGeo.setAttribute('color',new THREE.BufferAttribute(col,3));
  const dust=new THREE.Points(dustGeo,new THREE.PointsMaterial({map:particleMap,size:1.15,vertexColors:true,transparent:true,opacity:.92,depthWrite:false,blending:THREE.AdditiveBlending,sizeAttenuation:true,alphaTest:.01}));disk.add(dust);
  const jetPos=new Float32Array(360*3),jetCol=new Float32Array(360*3);for(let i=0;i<360;i++){const side=i%2?1:-1,y=7+Math.random()*23,r=(1-y/34)*(.7+Math.random()*1.2);jetPos.set([(Math.random()-.5)*r,side*y,(Math.random()-.5)*r],i*3);const q=new THREE.Color(side>0?0x8fdcff:0xffa05f);jetCol.set([q.r,q.g,q.b],i*3);}
  const jetGeo=new THREE.BufferGeometry();jetGeo.setAttribute('position',new THREE.BufferAttribute(jetPos,3));jetGeo.setAttribute('color',new THREE.BufferAttribute(jetCol,3));hole.add(new THREE.Points(jetGeo,new THREE.PointsMaterial({map:particleMap,size:1.4,vertexColors:true,transparent:true,opacity:.42,depthWrite:false,blending:THREE.AdditiveBlending,alphaTest:.01})));
  const halo=new THREE.Sprite(new THREE.SpriteMaterial({map:particleMap,color:0xff6c2e,transparent:true,opacity:.23,blending:THREE.AdditiveBlending,depthWrite:false}));halo.scale.set(42,42,1);hole.add(halo);
  const cam={theta:0,phi:.4,radius:61,targetTheta:0,targetPhi:.4,targetRadius:61,drag:false,x:0,y:0};
  function updateCamera(){cam.theta+=(cam.targetTheta-cam.theta)*.08;cam.phi+=(cam.targetPhi-cam.phi)*.08;cam.radius+=(cam.targetRadius-cam.radius)*.08;camera.position.set(cam.radius*Math.sin(cam.phi)*Math.sin(cam.theta),cam.radius*Math.cos(cam.phi),cam.radius*Math.sin(cam.phi)*Math.cos(cam.theta));camera.lookAt(target);}
  function move(x,y){const dx=x-cam.x,dy=y-cam.y;cam.x=x;cam.y=y;cam.targetTheta-=dx*.004;cam.targetPhi=Math.max(.12,Math.min(1.25,cam.targetPhi-dy*.003));}
  canvas.addEventListener('mousedown',e=>{cam.drag=true;cam.x=e.clientX;cam.y=e.clientY;});window.addEventListener('mousemove',e=>{if(cam.drag)move(e.clientX,e.clientY);});window.addEventListener('mouseup',()=>{cam.drag=false;});
  canvas.addEventListener('wheel',e=>{e.preventDefault();cam.targetRadius=Math.max(34,Math.min(110,cam.targetRadius+e.deltaY*.045));},{passive:false});
  canvas.addEventListener('touchstart',e=>{if(e.touches.length===1){cam.drag=true;cam.x=e.touches[0].clientX;cam.y=e.touches[0].clientY;}},{passive:true});canvas.addEventListener('touchmove',e=>{if(cam.drag&&e.touches.length===1)move(e.touches[0].clientX,e.touches[0].clientY);},{passive:true});canvas.addEventListener('touchend',()=>{cam.drag=false;},{passive:true});
  let warp=0;const form=document.querySelector('.control-panel');if(form)form.addEventListener('submit',()=>{warp=1;});
  const clock=new THREE.Clock();
  function animate(){requestAnimationFrame(animate);if(document.hidden)return;const dt=Math.min(clock.getDelta(),.05),t=clock.getElapsedTime();for(let i=0;i<n;i++){ang[i]+=dt*spd[i];pos[i*3]=Math.cos(ang[i])*rad[i];pos[i*3+2]=Math.sin(ang[i])*rad[i];pos[i*3+1]=Math.sin(ang[i]*2.3+i)*(.12+1.3*rad[i]/22);}dustGeo.attributes.position.needsUpdate=true;photon.rotation.z=t*.12;lens.rotation.z=-t*.08;outer.rotation.z=t*.045;disk.rotation.y=t*.045;starsFar.rotation.y=t*.002;photon.material.opacity=.78+Math.sin(t*2.1)*.18;lens.material.opacity=.3+Math.sin(t*1.4)*.1;halo.material.opacity=.2+Math.sin(t*1.1)*.06;if(!cam.drag)cam.targetTheta+=dt*.025;if(warp){warp+=dt*1.5;cam.targetRadius=61-Math.min(warp,1)*25;if(warp>1.4)warp=0;}updateCamera();renderer.render(scene,camera);}
  updateCamera();animate();addEventListener('resize',()=>{camera.aspect=innerWidth/innerHeight;camera.updateProjectionMatrix();renderer.setSize(innerWidth,innerHeight);});
  const state=document.getElementById('scene-state');if(state)state.textContent='3D 黑洞场景 · 黑洞吸积盘 / 拖动旋转';
} catch(error) {const state=document.getElementById('scene-state');if(state)state.textContent='静态黑洞模式 · 登录正常可用';console.warn('登录背景场景不可用，已保留登录功能',error);}
