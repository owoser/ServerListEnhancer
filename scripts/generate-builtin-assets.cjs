// Original deterministic pixel art. No game textures or third-party assets.
const fs=require('node:fs'), zlib=require('node:zlib'), path=require('node:path');
function canvas(w,h){const p=Buffer.alloc(w*h*4);return{w,h,p,set(x,y,c){x|=0;y|=0;if(x<0||y<0||x>=w||y>=h)return;const i=(y*w+x)*4;p[i]=c[0];p[i+1]=c[1];p[i+2]=c[2];p[i+3]=c.length>3?c[3]:255;}};}
const noise=(x,y,s=0)=>{let a=Math.imul(x+9271,374761393)^Math.imul(y+239,668265263)^s;a=Math.imul(a^(a>>>13),1274126177);return((a^(a>>>16))>>>0)%1000/1000;};
function rect(c,x,y,w,h,color){for(let j=y;j<y+h;j++)for(let i=x;i<x+w;i++)c.set(i,j,color);}
function poly(c,points,color){let ys=points.map(p=>p[1]);for(let y=Math.floor(Math.min(...ys));y<=Math.max(...ys);y++){const xs=[];for(let i=0,j=points.length-1;i<points.length;j=i++){let a=points[i],b=points[j];if((a[1]>y)!=(b[1]>y))xs.push((b[0]-a[0])*(y-a[1])/(b[1]-a[1])+a[0]);}xs.sort((a,b)=>a-b);for(let i=0;i<xs.length;i+=2)for(let x=Math.ceil(xs[i]);x<xs[i+1];x++)c.set(x,y,typeof color==='function'?color(x,y):color);}}
function png(c){const crc=b=>{let n=0xffffffff;for(const v of b){n^=v;for(let i=0;i<8;i++)n=(n>>>1)^((n&1)?0xedb88320:0);}return(n^0xffffffff)>>>0;};const chunk=(t,b)=>{const type=Buffer.from(t),len=Buffer.alloc(4),tail=Buffer.alloc(4);len.writeUInt32BE(b.length);tail.writeUInt32BE(crc(Buffer.concat([type,b])));return Buffer.concat([len,type,b,tail]);};const ih=Buffer.alloc(13);ih.writeUInt32BE(c.w);ih.writeUInt32BE(c.h,4);ih[8]=8;ih[9]=6;const rows=Buffer.alloc((c.w*4+1)*c.h);for(let y=0;y<c.h;y++)c.p.copy(rows,y*(c.w*4+1)+1,y*c.w*4,(y+1)*c.w*4);return Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]),chunk('IHDR',ih),chunk('IDAT',zlib.deflateSync(rows,{level:9})),chunk('IEND',Buffer.alloc(0))]);}
const logo=canvas(64,64);
poly(logo,[[7,21],[32,8],[57,21],[57,46],[32,59],[7,46]],[31,43,35]);
poly(logo,[[8,22],[32,34],[32,57],[8,45]],(x,y)=>{const n=noise(x>>1,y>>1);return n<.14?[149,105,62]:n<.36?[116,78,45]:[130,88,50];});
poly(logo,[[32,34],[56,22],[56,45],[32,57]],(x,y)=>{const n=noise(x>>1,y>>1,98);return n<.14?[106,76,47]:n<.38?[76,55,35]:[90,64,39];});
poly(logo,[[8,21],[32,9],[56,21],[32,33]],(x,y)=>{const n=noise(x>>1,y>>1,33);return n<.15?[150,196,76]:n<.45?[109,169,61]:[124,181,65];});
poly(logo,[[8,22],[32,34],[32,40],[27,37],[27,40],[22,37],[22,34],[17,32],[17,35],[12,32],[12,29],[8,27]],[83,145,54]);
poly(logo,[[32,34],[56,22],[56,27],[51,30],[51,33],[46,35],[46,32],[40,35],[40,39],[35,42],[35,38],[32,40]],[57,114,44]);
// Tiny stony flecks, unique hand-designed block pattern.
rect(logo,13,39,3,2,[173,138,92]);rect(logo,23,45,2,2,[82,63,44]);rect(logo,41,45,3,2,[114,94,67]);
const small=canvas(320,107);
for(let y=0;y<107;y++)rect(small,0,y,320,1,[Math.round(99+y*.75),Math.round(171+y*.36),Math.round(212+y*.13)]);
rect(small,255,13,12,12,[255,238,167]);rect(small,252,16,18,6,[255,238,167]);
for(const [x,y,w] of [[18,15,39],[91,25,24],[171,11,45],[281,33,34]]){rect(small,x,y,w,5,[239,246,231]);rect(small,x+8,y-3,w-16,4,[251,251,236]);rect(small,x+4,y+5,w-6,2,[206,226,218]);}
poly(small,[[0,66],[24,49],[42,53],[67,31],[87,50],[101,44],[122,66],[156,48],[181,54],[210,36],[228,52],[245,45],[276,63],[299,50],[320,61],[320,107],[0,107]],[94,143,148]);
poly(small,[[52,43],[67,31],[78,42],[70,40],[65,43],[61,40]],[221,235,222]);
poly(small,[[0,76],[19,66],[39,72],[55,59],[78,64],[103,76],[128,63],[158,70],[177,61],[202,68],[223,60],[244,75],[272,64],[296,70],[320,63],[320,107],[0,107]],[78,130,94]);
poly(small,[[0,80],[25,75],[48,80],[73,70],[98,76],[118,86],[143,76],[173,80],[200,74],[225,80],[258,76],[285,84],[320,77],[320,107],[0,107]],[111,157,73]);
poly(small,[[199,78],[211,79],[197,85],[182,88],[173,92],[189,98],[205,107],[138,107],[149,98],[163,92],[170,87],[191,83]],[70,149,175]);
poly(small,[[200,80],[204,80],[190,85],[177,89],[173,91],[176,93],[165,93],[170,87],[191,83]],[133,202,202]);
poly(small,[[0,96],[34,88],[63,91],[86,84],[112,91],[139,107],[0,107]],(x,y)=>{const n=noise(x>>1,y>>1,66);return n<.2?[89,130,53]:n<.55?[111,148,57]:[124,162,63];});
poly(small,[[224,97],[251,86],[280,89],[301,82],[320,87],[320,107],[208,107]],[82,124,58]);
function tree(x,y,s=1){rect(small,x,y-15*s,3*s,16*s,[90,73,48]);rect(small,x-6*s,y-24*s,15*s,10*s,[48,101,72]);rect(small,x-9*s,y-20*s,20*s,9*s,[55,116,72]);rect(small,x-5*s,y-27*s,12*s,7*s,[70,131,74]);rect(small,x-6*s,y-25*s,5*s,3*s,[96,151,85]);}
tree(27,95);tree(47,90);tree(281,89);tree(307,99);tree(103,80);
for(let i=0;i<50;i++){const x=(noise(i,8)*320)|0,y=96+((noise(i,10)*10)|0);if((x>145&&x<207))continue;small.set(x,y,i%3?[160,181,74]:[245,218,126]);small.set(x,y+1,[78,116,48]);}
const banner=canvas(960,320);for(let y=0;y<320;y++)for(let x=0;x<960;x++){const i=(((y/3)|0)*small.w+((x/3)|0))*4;banner.set(x,y,[...small.p.subarray(i,i+4)]);}
fs.mkdirSync('resources/logo',{recursive:true});fs.mkdirSync('resources/intro',{recursive:true});
fs.writeFileSync('resources/logo/default.png',png(logo));fs.writeFileSync('resources/intro/default.png',png(banner));
let cpp='#include "BuiltinImages.h"\n#ifdef SLE_CLIENT\nnamespace sle::client::builtin {\n';
for(const [name,file] of [['logo','resources/logo/default.png'],['banner','resources/intro/default.png']]){const bytes=fs.readFileSync(file);cpp+='static constexpr unsigned char '+name+'Bytes[]={\n';for(let i=0;i<bytes.length;i+=24)cpp+=[...bytes.subarray(i,i+24)].join(',')+',\n';cpp+='};\nstd::span<unsigned char const> '+name+'(){return '+name+'Bytes;}\n';console.log(file,bytes.length);}
cpp+='}\n#endif\n';fs.writeFileSync('src/common/BuiltinImages.cpp',cpp);
