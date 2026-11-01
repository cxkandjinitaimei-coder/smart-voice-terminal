/* 本地WebGL光路弯折近似：积分轨迹与薄吸积盘相交后累积发光。
 * Schwarzschild近似，不是影视原图或科学仿真。登录场景，无远端请求。
 */
(() => {
  'use strict';
  const canvas=document.getElementById('blackhole'),state=document.getElementById('render-state'),button=document.getElementById('pause-motion');
  const tiltInput=document.getElementById('tilt-angle'),tiltValue=document.getElementById('tilt-value');
  let tilt=Number(tiltInput.value)*Math.PI/180;
  let invalidate=()=>{};
  tiltInput.addEventListener('input',()=>{tilt=Number(tiltInput.value)*Math.PI/180;tiltValue.textContent=tiltInput.value+'°';document.getElementById('static-disc')?.setAttribute('transform','translate(520 440) rotate('+tiltInput.value+')');invalidate();});
  let paused=matchMedia('(prefers-reduced-motion: reduce)').matches;
  let gl,stopped=false;
  try {
    gl=canvas.getContext('webgl',{alpha:false,antialias:false,preserveDrawingBuffer:true,powerPreference:'low-power'});
    if(!gl)throw Error('WebGL unavailable');
    const vertex=`attribute vec2 a;void main(){gl_Position=vec4(a,0.,1.);}`;
    const fragment=`precision highp float;
uniform vec2 size;uniform float time;uniform float mobile;uniform float tilt;
float hash(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}
float noise(vec2 p){vec2 i=floor(p),f=fract(p);f=f*f*(3.-2.*f);return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);}
vec3 sky(vec3 d){vec2 uv=vec2(atan(d.z,d.x),asin(clamp(d.y,-1.,1.)))*vec2(130.,190.);vec2 cell=floor(uv);float h=hash(cell);vec2 f=fract(uv)-.5;float star=pow(max(0.,1.-length(f)*2.5),10.)*step(.993,h);float haze=pow(noise(uv*.035),4.)*.011;return vec3(.006,.009,.015)+vec3(.8,.86,1.)*star*.8+vec3(.6,.46,.32)*haze;}
vec3 disk(vec3 p,vec3 direction){
 float r=length(p.xz),a=atan(p.z,p.x);
 float fall=smoothstep(2.75,3.25,r)*(1.-smoothstep(5.2,9.6,r));
 // 相同半径的亮斑沿角方向运动，内盘更快；不是随机闪烁一张静态图。
 float flow=a-time*(.6+pow(3./r,1.5));
 float clump=noise(vec2(cos(flow)*5.+r*.25,sin(flow)*5.+r*.75));
 float streak=.45+.23*sin(r*28.+sin(flow*6.+r*.8)*2.)+.12*sin(r*70.);
 float swirl=smoothstep(.25,.78,clump);
 float doppler=clamp(1.-p.x/r*.55,.45,1.6);
 vec3 heat=mix(vec3(1.,.32,.07),vec3(1.,.87,.62),clamp(1.-(r-3.)/6.,0.,1.));
 return heat*fall*(.12+streak*.75+swirl*.9)*doppler*1.5;
}
void main(){
 vec2 uv=(gl_FragCoord.xy-.5*size)/size.y;
 uv.x+=mix(.30,0.,mobile);uv.y+=mix(.105,-.11,mobile);
 uv*=mix(1.,2.6,mobile);
 float c=cos(tilt),s=sin(tilt);
 uv=mat2(c,-s,s,c)*uv;
 vec3 ro=vec3(0.,2.3,19.5),target=vec3(0.,0.,0.);
 vec3 fw=normalize(target-ro),right=normalize(cross(fw,vec3(0.,1.,0.))),up=cross(right,fw);
 vec3 rd=normalize(fw+uv.x*right*1.14+uv.y*up*1.14);
 vec3 p=ro,color=vec3(0.);float h2=dot(cross(p,rd),cross(p,rd));float transmission=1.;float closest=100.;bool swallowed=false;
 for(int i=0;i<170;i++){
   float r=length(p);closest=min(closest,r);
   if(r<1.025){swallowed=true;break;}
   if(r>35.)break;
   float ds=clamp((r-1.)*.12,.035,.65);
   vec3 prev=p;
   vec3 acceleration=-1.5*h2*p/pow(r,5.);
   rd=normalize(rd+acceleration*ds);
   p+=rd*ds;
   if(prev.y*p.y<0.){
     float f=prev.y/(prev.y-p.y);vec3 hit=mix(prev,p,f);float radius=length(hit.xz);
     if(radius>2.78&&radius<9.7){color+=transmission*disk(hit,rd);transmission*=.24;}
   }
 }
 if(!swallowed)color+=sky(rd)*transmission;
 // 轮廓外的稀薄散射光；不向黑洞内叠发光，保持暗色剪影。
 float limb=exp(-abs(closest-1.52)*12.)*.13;
 if(!swallowed)color+=vec3(1.,.65,.28)*limb;
 color=vec3(1.)-exp(-color*1.45);
 color=pow(color,vec3(.82));
 float vignette=1.-smoothstep(.2,1.25,length(uv*vec2(.6,1.)))*.45;
 color*=vignette;
 float grain=(hash(gl_FragCoord.xy+fract(time)*20.)-.5)*.012;
 gl_FragColor=vec4(max(color+grain,0.),1.);
}`;
    function compile(type,source){const shader=gl.createShader(type);gl.shaderSource(shader,source);gl.compileShader(shader);if(!gl.getShaderParameter(shader,gl.COMPILE_STATUS))throw Error(gl.getShaderInfoLog(shader));return shader;}
    const program=gl.createProgram();gl.attachShader(program,compile(gl.VERTEX_SHADER,vertex));gl.attachShader(program,compile(gl.FRAGMENT_SHADER,fragment));gl.linkProgram(program);if(!gl.getProgramParameter(program,gl.LINK_STATUS))throw Error(gl.getProgramInfoLog(program));gl.useProgram(program);
    const buffer=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,buffer);gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([-1,-1,1,-1,-1,1,-1,1,1,-1,1,1]),gl.STATIC_DRAW);const attribute=gl.getAttribLocation(program,'a');gl.enableVertexAttribArray(attribute);gl.vertexAttribPointer(attribute,2,gl.FLOAT,false,0,0);
    const uniforms={size:gl.getUniformLocation(program,'size'),time:gl.getUniformLocation(program,'time'),mobile:gl.getUniformLocation(program,'mobile'),tilt:gl.getUniformLocation(program,'tilt')};
    let elapsed=0,last=0,needsFrame=true,frames=0;
    invalidate=()=>{needsFrame=true;};
    function resize(){const scale=Math.min(1,1100/innerWidth);canvas.width=Math.round(innerWidth*scale);canvas.height=Math.round(innerHeight*scale);gl.viewport(0,0,canvas.width,canvas.height);gl.uniform2f(uniforms.size,canvas.width,canvas.height);gl.uniform1f(uniforms.mobile,innerWidth<=650?1:0);needsFrame=true;}
    function draw(now){if(stopped)return;requestAnimationFrame(draw);if(document.hidden)return;if(now-last<40&&!needsFrame)return;const dt=Math.min((now-last)/1000,.25);last=now;if(paused&&!needsFrame)return;elapsed+=paused?0:dt;gl.uniform1f(uniforms.time,elapsed);gl.uniform1f(uniforms.tilt,tilt);gl.drawArrays(gl.TRIANGLES,0,6);if(gl.getError()!==gl.NO_ERROR){fallback();return;}frames++;needsFrame=false;document.querySelector('.cosmos').classList.add('ready');canvas.dataset.ready='true';}
    function update(){button.textContent=paused?'播放动效':'暂停动效';button.setAttribute('aria-pressed',String(paused));state.textContent=paused?'实时 WebGL · 已暂停':'实时 WebGL · 吸积盘旋转中';window.AVATAR_MOTION?.setEnabled(!paused);}
    button.addEventListener('click',()=>{paused=!paused;if(!paused)needsFrame=true;update();});
    canvas.addEventListener('webglcontextlost',event=>{event.preventDefault();fallback();});
    matchMedia('(prefers-reduced-motion: reduce)').addEventListener('change',event=>{paused=event.matches;if(!paused)needsFrame=true;update();});
    addEventListener('resize',resize);resize();update();requestAnimationFrame(draw);
    window.BLACKHOLE_SCENE={get paused(){return paused;},get ready(){return canvas.dataset.ready==='true'&&!stopped;},get frames(){return frames;},get time(){return elapsed;},get tilt(){return Number(tiltInput.value);},illustration:true};
  }catch(error){fallback();}
  function fallback(){stopped=true;canvas.hidden=true;state.textContent='静态引力光环 · WebGL 不可用';button.hidden=true;}
})();
