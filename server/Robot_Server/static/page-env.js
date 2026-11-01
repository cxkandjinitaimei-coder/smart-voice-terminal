/* Real environment data in one smooth chart. No raw-history polling. */
(function(){
  'use strict';
  const D=window.DASH_API;
  let hours=3,chart=null,requestVersion=0;
  function tile(label,value,note){return '<div class="stat"><span class="k">'+D.escapeHtml(label)+'</span><span class="v">'+D.escapeHtml(value)+'</span><small>'+D.escapeHtml(note)+'</small></div>';}
  function stats(s){
    const now=s.now||(s.points||[]).at(-1),range=(v,unit)=>v&&v.min!=null?D.fmtNum(v.min,1)+' ~ '+D.fmtNum(v.max,1)+unit:'尚无采样';
    D.$('#env-stats').innerHTML=tile('温度',now?D.fmtNum(now.temp,1)+' °C':'—',range(s.temp,' °C'))+tile('湿度',now?D.fmtNum(now.humi,1)+' %':'—',range(s.humi,' %'))+tile('有效采样',String(s.samples||0)+' 条',now?'更新于 '+D.timeShort(now.ts):'等待设备上报');
  }
  function minutePoints(input){
    const result=[];
    for(const p of input||[]){const previous=result.at(-1),minute=p.epoch>0?Math.floor(p.epoch/60):String(p.ts).slice(0,16);if(previous&&previous.minute===minute)result[result.length-1]={...p,minute};else result.push({...p,minute});}
    const intervals=result.slice(1).map((p,i)=>p.epoch-result[i].epoch).filter(n=>n>0).sort((a,b)=>a-b);
    const threshold=Math.max(300,(intervals[Math.floor(intervals.length/2)]||60)*3),plotted=[];
    for(const p of result){const previous=plotted.at(-1);if(previous&&p.epoch-previous.epoch>threshold)plotted.push({ts:'无采样',temp:null,humi:null});plotted.push(p);}
    return plotted;
  }
  function trend(s){
    const points=minutePoints(s.points),labels=points.map(p=>p.ts);
    const series=[{label:'温度 °C',color:'--temp',data:points.map(p=>p.temp),fill:true,pointRadius:0,pointHoverRadius:4,smooth:true,spanGaps:false},{label:'湿度 %',color:'--humi',data:points.map(p=>p.humi),fill:false,pointRadius:0,pointHoverRadius:4,smooth:true,yAxisID:'humidity',spanGaps:false}];
    if(!chart){chart=D.mkLineChart('#chart-temp',labels,series,{legend:true,empty:'等待设备上传环境数据',maxTicks:7,unit:'°C'});chart.options.scales.humidity={position:'right',grid:{drawOnChartArea:false},border:{display:false},ticks:{color:getComputedStyle(document.documentElement).getPropertyValue('--muted').trim()},title:{display:true,text:'%'}};chart.options.scales.x.ticks.callback=function(value){const text=this.getLabelForValue(value);return hours>24?String(text).slice(5,16):D.timeShort(text);};chart.update('none');}
    else D.setChartData(chart,labels,series);
    const empty=D.$('#env-empty');empty.classList.toggle('hidden',!!s.samples);empty.textContent=s.samples?'':'本时段暂无有效采样，可切换时间范围。';
  }
  function init(){D.$('#range-tabs').addEventListener('click',event=>{const b=event.target.closest('[data-hours]');if(!b)return;hours=Number(b.dataset.hours)||3;D.$$('#range-tabs .rtab').forEach(n=>n.classList.toggle('active',n===b));refresh();});}
  async function refresh(){const version=++requestVersion,s=await D.getJSON('/api/stats/sensor?hours='+hours+'&limit=400');if(version!==requestVersion)return;if(s.error)throw Error(s.error);stats(s);trend(s);D.markUp();}
  window.PAGES.env={title:'环境记录',init,refresh};
})();
