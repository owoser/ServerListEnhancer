// Tiny deterministic GIF89a fixtures, not third-party art.
const fs=require('node:fs');fs.mkdirSync('build/gif-fixtures',{recursive:true});
const u16=n=>[n&255,n>>8];
function gif(frames,{w=3,h=2,loop=2}={}){
 let b=[...Buffer.from('GIF89a'),...u16(w),...u16(h),0x81,0,0,0,0,0,255,0,0,0,255,0,0,0,255];
 if(loop!==null)b.push(0x21,0xff,11,...Buffer.from('NETSCAPE2.0'),3,1,...u16(loop),0);
 for(const f of frames){let {x=0,y=0,width=w,height=h,disposal=1,delay=10,pixels,transparent=true,palette}=f;
  b.push(0x21,0xf9,4,(disposal<<2)|(transparent?1:0),...u16(delay),0,0);
  b.push(0x2c,...u16(x),...u16(y),...u16(width),...u16(height),palette?0x81:0);if(palette)b.push(...palette);
  // Emit clear before each pixel so code width stays at three bits.
  let bits=[];for(const p of pixels)for(const c of [4,p])for(let j=0;j<3;j++)bits.push((c>>j)&1);for(let j=0;j<3;j++)bits.push((5>>j)&1);
  const data=[];for(let k=0;k<bits.length;k+=8){let v=0;for(let j=0;j<8;j++)v|=(bits[k+j]||0)<<j;data.push(v);}b.push(2,data.length,...data,0);
 }b.push(0x3b);return Buffer.from(b);
}
const frames=[{pixels:[1,1,1,1,1,1]},{x:0,y:0,width:1,height:1,disposal:2,pixels:[2]},{x:2,y:0,width:1,height:1,disposal:3,pixels:[3]},{x:1,y:0,width:1,height:1,pixels:[2]}];
fs.writeFileSync('build/gif-fixtures/disposal.gif',gif(frames));
fs.writeFileSync('build/gif-fixtures/local-palette.gif',gif([{pixels:[2,2,2,2,2,2],palette:[0,0,0,255,0,0,255,255,0,0,0,255]}],{loop:null}));
fs.writeFileSync('build/gif-fixtures/zero-delay.gif',gif([{pixels:[1,1,1,1,1,1],delay:0}]));
fs.writeFileSync('build/gif-fixtures/too-many.gif',gif(Array.from({length:121},()=>({pixels:[1,1,1,1,1,1]}))));
fs.writeFileSync('build/gif-fixtures/too-long.gif',gif([{pixels:[1,1,1,1,1,1],delay:4000}]));
const oversized=gif(frames);oversized.writeUInt16LE(1025,6);fs.writeFileSync('build/gif-fixtures/too-wide.gif',oversized);
console.log('Synthetic disposal/loop/palette/delay/budget GIF fixtures generated');
