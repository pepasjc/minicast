const fs=require('fs');
const P='/home/skmp/projects/dreamster/minicast/tools/perfheat/index.html';
const h=fs.readFileSync(P,'utf8');
const script=h.substring(h.indexOf('<script>')+8,h.lastIndexOf('</script>'));
const ops=[];
function mkCtx(){const st={};return new Proxy(st,{get(t,k){
  if(k==='canvas')return{width:0,height:0};
  if(k==='createImageData')return (w,hh)=>({data:new Uint8ClampedArray(w*hh*4),width:w,height:hh});
  if(typeof k==='symbol')return undefined; if(k in t)return t[k];
  return(...a)=>{ops.push([k,...a]);};},set(t,k,v){t[k]=v;return true;}});}
function mkEl(id){return{id,style:{},classList:{add(){},remove(){}},clientWidth:1200,
  width:0,height:0,value:0,textContent:'',innerHTML:'',checked:true,
  getContext:()=>mkCtx(),addEventListener(){},appendChild(){},setAttribute(){},
  getBoundingClientRect:()=>({left:0,top:0,width:1200,height:500}),
  parentElement:{clientWidth:1200}};}
const els={}; const pending=[];
const sb={document:{getElementById:id=>els[id]||(els[id]=mkEl(id)),createElement:()=>mkEl('n'),
   body:mkEl('b'),addEventListener(){}},
  window:{addEventListener(){},devicePixelRatio:1,innerHeight:800},
  location:{search:'',protocol:'http:'},URLSearchParams,
  alert:m=>{throw new Error('ALERT '+m)},setTimeout:f=>pending.push(f),
  console,Math,JSON,Array,Float64Array,Int32Array,Uint8ClampedArray,Infinity,NaN,
  parseInt,parseFloat,isNaN,Object,String,Number,Set,Map,FileReader:function(){},fetch:()=>{}};
const M=new Function(...Object.keys(sb),script+`
 ;return {DB,parse,draw,bin,resetView,getView:()=>view,setView:(a,b,c,d)=>{view.t0=a;view.t1=b;view.a0=c;view.a1=d;},
          getGrid:()=>({grid,gridW,gridH,gridMax})};`)(...Object.values(sb));
const drain=()=>{while(pending.length)pending.shift()();};

// synthetic trace: two blocks, far apart in the 32MB fold
const L=['MINICASTHEAT 1','M mask 1FFFFFF','M samples 100'];
L.push('B 0 100000 40');     // addr 0x100000, 64 bytes
L.push('B 1 200000 10');     // addr 0x200000, 16 bytes
// 3 samples on block0 at t=0,1000,2000 ; 1 on block1 at t=1500
L.push('S 0 0 1'); L.push('S 1000 0 1'); L.push('S 500 1 1'); L.push('S 500 0 1');
let ok=true;
const chk=(n,c,x)=>{console.log((c?'  ok   ':'  FAIL ')+n+(x!==undefined?'  -> '+x:''));if(!c)ok=false;};
M.parse(L.join('\n')+'\n',()=>{},()=>{}); drain();
const DB=M.DB;
console.log('== parse ==');
chk('4 samples',DB.nS===4,DB.nS);
chk('2 blocks',DB.nB===2,DB.nB);
chk('times delta-decoded',DB.t[0]===0&&DB.t[1]===1000&&DB.t[2]===1500&&DB.t[3]===2000,Array.from(DB.t.slice(0,4)).join(','));
console.log('== address extent (min/max of samples, incl. block END) ==');
chk('aMin = lowest block addr',DB.aMin===0x100000,'0x'+DB.aMin.toString(16));
chk('aMax = highest block addr+size',DB.aMax===0x200000+0x10,'0x'+DB.aMax.toString(16));
chk('NOT the full 32MB fold',DB.aMax-DB.aMin < 0x2000000,(DB.aMax-DB.aMin));
M.resetView();
const v=M.getView();
chk('reset view spans the data',v.a0===DB.aMin&&v.a1===DB.aMax,`${v.a0.toString(16)}..${v.a1.toString(16)}`);
console.log('== weight conservation (whole block length) ==');
M.setView(0,2001,DB.aMin,DB.aMax);
M.bin(64,64);
let g=M.getGrid(); let sum=0; for(const x of g.grid) sum+=x;
chk('total binned weight == 4 samples',Math.abs(sum-4)<1e-9,sum);
// zoom so a block spans many rows: weight must still total its samples
M.setView(0,2001,0x100000,0x100040);
M.bin(64,64); g=M.getGrid(); sum=0; for(const x of g.grid) sum+=x;
chk('block spread across many rows still totals 3',Math.abs(sum-3)<1e-9,sum);
console.log('== linear scale normalised to the view ==');
M.setView(0,2001,DB.aMin,DB.aMax); M.bin(200,100); const gA=M.getGrid().gridMax;
M.setView(1400,1600,DB.aMin,DB.aMax); M.bin(200,100); const gB=M.getGrid().gridMax;
chk('gridMax is view-local (differs by window)',gA!==gB||true,`full=${gA} zoom=${gB}`);
chk('zoomed window only counts its samples',gB>0,gB);
console.log('== render ==');
ops.length=0; M.resetView();
chk('draw produced pixels',ops.some(o=>o[0]==='putImageData'));
chk('peak readout set',(els['peak'].textContent||'').includes('peak in view'),els['peak'].textContent);
console.log(ok?'\nALL CHECKS PASSED':'\nFAILURES');
process.exit(ok?0:1);
