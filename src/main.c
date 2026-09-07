#include "adc_dma.h"

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/version.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

#include <zephyr/debug/thread_analyzer.h>

#define HTTP_PORT 8080
#define HTTP_LISTEN_BACKLOG 8
#define HTTP_RECEIVE_TIMEOUT_MS 500
#define WEBSCOPE_VERSION "0.2.2"
#define WEBSCOPE_STREAM_MODE "FFT1+SCP1 multiplex"
#define RX_SIZE 512
#define SCOPE_POINTS 100
#define SCOPE_TRIGGER_POINT 25
#define SCOPE_DEFAULT_STEP 8
#define SPECTRUM_FAST_POINTS 1024
#define SPECTRUM_HIRES_POINTS 2048
#define SPECTRUM_MAX_POINTS SPECTRUM_HIRES_POINTS
#define SPECTRUM_CHUNK_POINTS 256
#define SPECTRUM_DECIMATION_20KHZ 10
#define FFT_BINARY_MAGIC 0x31544646u /* "FFT1" Little Endian */
#define SCOPE_BINARY_MAGIC 0x31504353u /* "SCP1" Little Endian */
#define EVENT_SIZE 2048

static int event_client = -1;
static K_MUTEX_DEFINE(event_lock);
K_MSGQ_DEFINE(fft_client_queue, sizeof(int), 1, 4);

static uint16_t scope_samples[SCOPE_POINTS];
static uint16_t scope_stream_samples[SCOPE_POINTS];
static uint32_t scope_sample_rate;
static uint32_t scope_generation;
static uint16_t scope_minimum;
static uint16_t scope_maximum;
static uint16_t scope_average;
static uint32_t scope_frequency_millihz;
static atomic_t scope_requested_step = ATOMIC_INIT(SCOPE_DEFAULT_STEP);
static atomic_t scope_stream_enabled = ATOMIC_INIT(1);
static uint32_t scope_step_used = SCOPE_DEFAULT_STEP;
static uint16_t spectrum_samples[SPECTRUM_MAX_POINTS];
static uint16_t spectrum_tx_samples[SPECTRUM_MAX_POINTS];
static uint16_t spectrum_stream_samples[SPECTRUM_MAX_POINTS];
static uint32_t spectrum_sample_rate;
static uint32_t spectrum_point_count;
static uint32_t spectrum_generation;
static atomic_t spectrum_requested_decimation = ATOMIC_INIT(1);
static uint16_t spectrum_hires_build[SPECTRUM_HIRES_POINTS];
static size_t spectrum_hires_count;
static uint32_t spectrum_decimation_sum;
static uint32_t spectrum_decimation_count;
static uint32_t spectrum_active_decimation = 1;
static uint16_t slow_scope_ring[SCOPE_POINTS];
static size_t slow_scope_write;
static size_t slow_scope_count;
static uint32_t slow_scope_sum;
static uint32_t slow_scope_samples_in_sum;
static uint32_t slow_scope_step;
static K_MUTEX_DEFINE(scope_lock);

struct __attribute__((packed)) fft_binary_header {
    uint32_t magic;
    uint32_t sequence;
    uint32_t sample_rate;
    uint16_t sample_count;
    uint16_t reserved;
};

struct __attribute__((packed)) scope_binary_header {
    uint32_t magic;
    uint32_t sequence;
    uint32_t sample_rate;
    uint16_t sample_count;
    uint16_t reserved0;
    uint32_t step;
    uint32_t frequency_millihz;
    uint16_t minimum;
    uint16_t maximum;
    uint16_t average;
    uint16_t reserved1;
};

/*
 * Die vollständige Webseite liegt im Flash des Pico.
 * Der Browser führt die Zeichenoperationen auf dem Canvas aus.
 */
static const char index_html[] =
"<!doctype html>"
"<html lang='de'>"
"<head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Zephyr WebDisplay</title>"
"<style>"
"body{"
"margin:0;"
"background:#20242b;"
"color:#ddd;"
"font:16px sans-serif;"
"display:grid;"
"place-items:center;"
"min-height:100vh"
"}"
"main{width:min(94vw,900px)}"
"canvas{"
"width:100%;"
"height:auto;"
"background:#000;"
"border:1px solid #68717d"
"}"
"#state{margin:.5rem 0;color:#8fd3ff}"
"#timebase{display:flex;gap:.5rem;flex-wrap:wrap;margin:.7rem 0}"
".controls{display:flex;gap:.5rem;flex-wrap:wrap;align-items:center;margin:.7rem 0}"
"button{background:#173047;color:#dff;border:1px solid #5d829f;"
"padding:.45rem .8rem;border-radius:4px;cursor:pointer}"
"button:hover{background:#245070}"
"select{background:#173047;color:#dff;border:1px solid #5d829f;padding:.4rem}"
"</style>"
"</head>"
"<body>"
"<main>"
"<h2>Zephyr WebDisplay</h2>"
"<div id='state'>Verbinde ...</div>"
"<canvas id='display' width='800' height='480'></canvas>"

"<h2>ADC Oscilloscope</h2>"
"<div class='controls'>"
"<button id='scopeToggle' onclick='toggleScopeStream()'>Scope einschalten</button>"
"<span id='scopeState'>aus</span>"
"</div>"
"<div id='timebase'>"
"<button onclick='setTimebase(1)'>25 us/div · 8 kHz</button>"
"<button onclick='setTimebase(2)'>50 us/div · 4 kHz</button>"
"<button onclick='setTimebase(4)'>100 us/div · 2 kHz</button>"
"<button onclick='setTimebase(8)'>200 us/div · 1 kHz</button>"
"<button onclick='setTimebase(80)'>2 ms/div · 100 Hz</button>"
"<button onclick='setTimebase(800)'>20 ms/div · 10 Hz</button>"
"<button onclick='setTimebase(8000)'>200 ms/div · 1 Hz</button>"
"<button onclick='setTimebase(80000)'>2 s/div · 0.1 Hz</button>"
"</div>"
"<canvas id='scope' width='800' height='360'></canvas>"
"<div id='scopeInfo'>Noch keine ADC-Daten</div>"

"<h2>ADC Spectrum</h2>"
"<div class='controls'>"
"<label>Mittelung <select onchange='setAveraging(this.value)'>"
"<option value='1'>Aus</option><option value='4' selected>4</option>"
"<option value='8'>8</option><option value='16'>16</option>"
"</select></label>"
"<button onclick='togglePeakHold()'>Peak-Hold Ein/Aus</button>"
"<button onclick='clearPeakHold()'>Peak löschen</button>"
"</div>"
"<div class='controls'>"
"<span>Frequenz:</span>"
"<button onclick='setSpectrumMax(200000)'>200 kHz</button>"
"<button onclick='setSpectrumMax(100000)'>100 kHz</button>"
"<button onclick='setSpectrumMax(50000)'>50 kHz</button>"
"<button onclick='setSpectrumMax(20000)'>20 kHz</button>"
"</div>"
"<canvas id='spectrum' width='800' height='360'></canvas>"
"<div id='spectrumInfo'>Noch keine FFT-Daten</div>"

"<h2>FFT Waterfall</h2>"
"<canvas id='waterfall' width='800' height='360'></canvas>"

"<h2>ADC Control</h2>"
"<div class='controls'>"
"<button onclick=\"adcCommand('start')\">Start</button>"
"<button onclick=\"adcCommand('stop')\">Stop</button>"
"<button onclick=\"adcCommand('single')\">Single</button>"
"<span id='adcState'>bereit</span>"
"</div>"

"<h2>Browser Clock</h2>"
"<canvas id='clock' width='800' height='260'></canvas>"

"</main>"
"<script>"
"const canvas=document.getElementById('display');"
"const ctx=canvas.getContext('2d');"
"const state=document.getElementById('state');"

"const scopeCanvas=document.getElementById('scope');"
"const scopeCtx=scopeCanvas.getContext('2d');"
"const scopeInfo=document.getElementById('scopeInfo');"
"const scopeToggle=document.getElementById('scopeToggle');"
"const scopeState=document.getElementById('scopeState');"

"const spectrumCanvas=document.getElementById('spectrum');"
"const spectrumCtx=spectrumCanvas.getContext('2d');"
"const spectrumInfo=document.getElementById('spectrumInfo');"
"const waterfallCanvas=document.getElementById('waterfall');"
"const waterfallCtx=waterfallCanvas.getContext('2d');"
"const adcState=document.getElementById('adcState');"
"let spectrumSequence=-1;"
"let spectrumChunks=new Array(4).fill(null);"
"let averagingCount=4;"
"let spectrumHistory=[];"
"let spectrumHistoryWrite=0,spectrumHistoryFrames=0;"
"let peakHoldEnabled=false;"
"let peakHold=[];"
"let spectrumMaxHz=200000;"
"let lastBinarySpectrumSequence=-1;"
"let fftReal=new Float64Array(0),fftImag=new Float64Array(0);"
"let fftPower=new Float64Array(0),fftMagnitude=new Float64Array(0);"
"let waterfallRow=null;"
"let fftFrameCounter=0,fftFps=0,fftFpsStarted=performance.now();"
"let fftReceiveBytes=new Uint8Array(4096);"
"let fftReceiveValues=new Uint16Array(fftReceiveBytes.buffer);"
"let scopeStreamEnabled=false;"
"let requestedScopeStep=8,fftStreamAbort=null;"

"function setAveraging(value){"
"averagingCount=Math.max(1,Number(value));spectrumHistory=[];"
"spectrumHistoryWrite=0;spectrumHistoryFrames=0;"
"}"
"function togglePeakHold(){peakHoldEnabled=!peakHoldEnabled;"
"spectrumInfo.textContent='Peak-Hold '+(peakHoldEnabled?'ein':'aus');}"
"function clearPeakHold(){if(peakHold.fill)peakHold.fill(0);else peakHold=[];}"
"async function setSpectrumMax(value){spectrumMaxHz=Number(value);"
"spectrumHistory=[];spectrumHistoryWrite=0;spectrumHistoryFrames=0;"
"peakHold=[];spectrumSequence=-1;"
"try{const response=await fetch('/spectrum?max='+spectrumMaxHz,{cache:'no-store'});"
"if(!response.ok)throw new Error('HTTP '+response.status);"
"}catch(error){console.error(error);spectrumInfo.textContent='FFT-Modus nicht gesetzt';}}"
"function formatTimeUs(value){"
"if(value>=1000000)return (value/1000000).toFixed(2)+' s';"
"if(value>=1000)return (value/1000).toFixed(2)+' ms';"
"return value.toFixed(1)+' us';"
"}"

"async function adcCommand(command){"
"adcState.textContent=command+' ...';"
"try{"
"const response=await fetch('/adc/'+command,{cache:'no-store'});"
"const text=await response.text();"
"adcState.textContent=text||command;"
"}catch(error){adcState.textContent='Fehler: '+error;console.error(error);}"
"}"

"function setTimebase(step){"
"requestedScopeStep=Number(step);"
"scopeInfo.textContent='Zeitbasis wird umgeschaltet ...';"
"if(fftStreamAbort)fftStreamAbort.abort();"
"}"

"const clockCanvas=document.getElementById('clock');"
"const clockCtx=clockCanvas.getContext('2d');"

"function fft(real,imag){"
"const n=real.length;"
"for(let i=1,j=0;i<n;i++){"
"let bit=n>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;"
"if(i<j){const rr=real[i],ii=imag[i];real[i]=real[j];imag[i]=imag[j];"
"real[j]=rr;imag[j]=ii;}"
"}"
"for(let length=2;length<=n;length<<=1){"
"const angle=-2*Math.PI/length;"
"const wr0=Math.cos(angle),wi0=Math.sin(angle);"
"for(let start=0;start<n;start+=length){"
"let wr=1,wi=0;"
"for(let j=0;j<length/2;j++){"
"const even=start+j,odd=even+length/2;"
"const tr=wr*real[odd]-wi*imag[odd];"
"const ti=wr*imag[odd]+wi*real[odd];"
"real[odd]=real[even]-tr;imag[odd]=imag[even]-ti;"
"real[even]+=tr;imag[even]+=ti;"
"const nextWr=wr*wr0-wi*wi0;wi=wr*wi0+wi*wr0;wr=nextWr;"
"}"
"}"
"}"
"}"

"function ensureSpectrumBuffers(n){"
"const bins=n/2;"
"if(fftReal.length!==n){"
"fftReal=new Float64Array(n);fftImag=new Float64Array(n);"
"fftPower=new Float64Array(bins);fftMagnitude=new Float64Array(bins);"
"peakHold=new Float64Array(bins);spectrumHistory=[];"
"spectrumHistoryWrite=0;spectrumHistoryFrames=0;"
"}"
"if(spectrumHistory.length!==averagingCount){"
"spectrumHistory=Array.from({length:averagingCount},()=>new Float64Array(bins));"
"spectrumHistoryWrite=0;spectrumHistoryFrames=0;"
"}"
"if(!waterfallRow||waterfallRow.width!==waterfallCanvas.width)"
"waterfallRow=waterfallCtx.createImageData(waterfallCanvas.width,1);"
"}"

"function drawSpectrum(values,rate){"
"const n=values.length;if(n<2)return;"
"ensureSpectrumBuffers(n);"
"const real=fftReal,imag=fftImag,power=fftPower,magnitude=fftMagnitude;"
"imag.fill(0);power.fill(0);magnitude.fill(0);"
"const mean=values.reduce((a,b)=>a+b,0)/n;"
"for(let i=0;i<n;i++){"
"const window=0.5-0.5*Math.cos(2*Math.PI*i/(n-1));"
"real[i]=(values[i]-mean)*window;"
"}"

"fft(real,imag);"
"const bins=n/2;"
"for(let i=1;i<bins;i++){"
"power[i]=real[i]*real[i]+imag[i]*imag[i];"
"}"
"spectrumHistory[spectrumHistoryWrite].set(power);"
"spectrumHistoryWrite=(spectrumHistoryWrite+1)%averagingCount;"
"spectrumHistoryFrames=Math.min(averagingCount,spectrumHistoryFrames+1);"
"for(let i=1;i<bins;i++){"
"let sum=0;for(let frame=0;frame<spectrumHistoryFrames;frame++)"
"sum+=spectrumHistory[frame][i];"
"magnitude[i]=Math.sqrt(sum/spectrumHistoryFrames);"
"}"
"if(peakHold.length!==bins)peakHold=new Float64Array(bins);"
"if(peakHoldEnabled){for(let i=1;i<bins;i++)peakHold[i]=Math.max(peakHold[i],magnitude[i]);}"
"const maxBin=Math.max(2,Math.min(bins-1,Math.floor(spectrumMaxHz*n/rate)));"
"let peak=1e-12,peakBin=1;"
"for(let i=1;i<=maxBin;i++){if(magnitude[i]>peak){peak=magnitude[i];peakBin=i;}}"
"const w=spectrumCanvas.width,h=spectrumCanvas.height;"
"spectrumCtx.fillStyle='#071018';spectrumCtx.fillRect(0,0,w,h);"
"spectrumCtx.strokeStyle='#243746';spectrumCtx.lineWidth=1;"
"for(let x=0;x<=w;x+=100){"
"spectrumCtx.beginPath();spectrumCtx.moveTo(x,0);spectrumCtx.lineTo(x,h);spectrumCtx.stroke();"
"}"
"for(let y=0;y<=h;y+=36){"
"spectrumCtx.beginPath();spectrumCtx.moveTo(0,y);spectrumCtx.lineTo(w,y);spectrumCtx.stroke();"
"}"
"spectrumCtx.fillStyle='#8fd3ff';spectrumCtx.font='14px monospace';"
"spectrumCtx.textAlign='left';spectrumCtx.fillText('0 dB',8,18);"
"spectrumCtx.fillText('-50 dB',8,h/2-5);spectrumCtx.fillText('-100 dB',8,h-8);"
"spectrumCtx.textAlign='center';"
"for(let division=0;division<=4;division++){"
"const frequency=spectrumMaxHz*division/4;"
"spectrumCtx.fillText((frequency/1000).toFixed(0)+' kHz',w*division/4,h-8);"
"}"
"spectrumCtx.strokeStyle='#ffd34e';spectrumCtx.lineWidth=2;"
"spectrumCtx.beginPath();"
"for(let i=1;i<=maxBin;i++){"
"let db=20*Math.log10(Math.max(magnitude[i],1e-12)/peak);"
"db=Math.max(-100,Math.min(0,db));"
"const x=(i-1)*(w-1)/(maxBin-1);const y=-db*h/100;"
"if(i===1)spectrumCtx.moveTo(x,y);else spectrumCtx.lineTo(x,y);"
"}"
"spectrumCtx.stroke();"
"if(peakHoldEnabled){"
"spectrumCtx.strokeStyle='#ff5555';spectrumCtx.lineWidth=1;spectrumCtx.beginPath();"
"for(let i=1;i<=maxBin;i++){"
"let db=20*Math.log10(Math.max(peakHold[i],1e-12)/peak);"
"db=Math.max(-100,Math.min(0,db));"
"const x=(i-1)*(w-1)/(maxBin-1),y=-db*h/100;"
"if(i===1)spectrumCtx.moveTo(x,y);else spectrumCtx.lineTo(x,y);"
"}spectrumCtx.stroke();}"
"waterfallCtx.drawImage(waterfallCanvas,0,0,w,waterfallCanvas.height-1,"
"0,1,w,waterfallCanvas.height-1);"
"const row=waterfallRow;"
"for(let x=0;x<w;x++){"
"const bin=1+Math.floor(x*(maxBin-1)/(w-1));"
"let db=20*Math.log10(Math.max(magnitude[bin],1e-12)/peak);"
"let t=Math.max(0,Math.min(1,(db+100)/100));"
"const p=x*4;row.data[p]=Math.round(255*Math.max(0,2*t-0.5));"
"row.data[p+1]=Math.round(255*Math.max(0,1-Math.abs(2*t-1)));"
"row.data[p+2]=Math.round(255*Math.max(0,1.5-2*t));row.data[p+3]=255;"
"}waterfallCtx.putImageData(row,0,0);"
"const peakFrequency=peakBin*rate/n;"
"fftFrameCounter++;const fpsNow=performance.now();"
"if(fpsNow-fftFpsStarted>=1000){"
"fftFps=fftFrameCounter*1000/(fpsNow-fftFpsStarted);"
"fftFrameCounter=0;fftFpsStarted=fpsNow;"
"}"
"spectrumInfo.textContent='FFT '+n+' Punkte | Auflösung '+"
"(rate/n).toFixed(1)+' Hz | Bereich 0-'+(spectrumMaxHz/1000).toFixed(0)+"
"' kHz | Spitze '+peakFrequency.toFixed(1)+' Hz | Mittelung '+averagingCount+"
"' | Peak-Hold '+(peakHoldEnabled?'ein':'aus')+"
"' | '+fftFps.toFixed(1)+' Bilder/s';"
"}"

"async function streamSpectrum(){"
"for(;;){try{"
"const controller=new AbortController();fftStreamAbort=controller;"
"const response=await fetch('/fft-stream?scope-step='+requestedScopeStep,"
"{cache:'no-store',signal:controller.signal});"
"if(!response.ok||!response.body)throw new Error('FFT-Stream HTTP '+response.status);"
"const reader=response.body.getReader();"
"const headerBytes=new Uint8Array(32);let headerUsed=0,payloadUsed=0;"
"let expectedBytes=0,magic=0,sequence=0,rate=0,count=0;"
"let step=0,frequency=0,minimum=0,maximum=0,average=0;"
"for(;;){const result=await reader.read();"
"if(result.done)throw new Error('FFT-Stream beendet');"
"const chunk=result.value;let offset=0;"
"while(offset<chunk.length){"
"if(headerUsed<32){const take=Math.min(32-headerUsed,chunk.length-offset);"
"headerBytes.set(chunk.subarray(offset,offset+take),headerUsed);"
"headerUsed+=take;offset+=take;if(headerUsed<32)continue;"
"const header=new DataView(headerBytes.buffer);"
"magic=header.getUint32(0,true);"
"if(magic!==0x31544646&&magic!==0x31504353)throw new Error('ungueltiger Stream-Header');"
"sequence=header.getUint32(4,true);rate=header.getUint32(8,true);"
"count=header.getUint16(12,true);step=header.getUint32(16,true);"
"frequency=header.getUint32(20,true);minimum=header.getUint16(24,true);"
"maximum=header.getUint16(26,true);average=header.getUint16(28,true);"
"expectedBytes=count*2;payloadUsed=0;"
"if(count<2||expectedBytes>4096)throw new Error('ungueltige Stream-Laenge');"
"if(fftReceiveBytes.length!==expectedBytes){"
"fftReceiveBytes=new Uint8Array(expectedBytes);"
"fftReceiveValues=new Uint16Array(fftReceiveBytes.buffer);"
"}"
"}"
"const take=Math.min(expectedBytes-payloadUsed,chunk.length-offset);"
"fftReceiveBytes.set(chunk.subarray(offset,offset+take),payloadUsed);"
"payloadUsed+=take;offset+=take;"
"if(payloadUsed===expectedBytes){"
"if(magic===0x31544646&&sequence!==lastBinarySpectrumSequence){"
"lastBinarySpectrumSequence=sequence;drawSpectrum(fftReceiveValues,rate);"
"}else if(magic===0x31504353&&scopeStreamEnabled){"
"executeCommand({cmd:'scope',rate:rate,step:step,min:minimum,max:maximum,"
"average:average,frequency_millihz:frequency,samples:fftReceiveValues});"
"}headerUsed=0;payloadUsed=0;expectedBytes=0;"
"}"
"}"
"}"
"}catch(error){if(error.name==='AbortError')continue;"
"console.error(error);spectrumInfo.textContent='FFT-Stream verbindet neu ...';"
"await new Promise(resolve=>setTimeout(resolve,500));}"
"}"
"}"

"function toggleScopeStream(){"
"scopeStreamEnabled=!scopeStreamEnabled;"
"scopeToggle.textContent=scopeStreamEnabled?'Scope ausschalten':'Scope einschalten';"
"scopeState.textContent=scopeStreamEnabled?'ein':'aus';"
"}"


"function executeCommand(command){"
"switch(command.cmd){"

"case 'clear':"
"ctx.fillStyle=command.color||'#000000';"
"ctx.fillRect(0,0,canvas.width,canvas.height);"
"break;"

"case 'line':"
"ctx.strokeStyle=command.color||'#00ff00';"
"ctx.lineWidth=command.width||1;"
"ctx.beginPath();"
"ctx.moveTo(command.x1,command.y1);"
"ctx.lineTo(command.x2,command.y2);"
"ctx.stroke();"
"break;"

"case 'text':"
"ctx.fillStyle=command.color||'#ffffff';"
"ctx.font=(command.size||28)+'px monospace';"
"ctx.fillText(command.text,command.x,command.y);"
"break;"

"case 'scope':{"
"const values=command.samples||[];"
"const w=scopeCanvas.width;"
"const h=scopeCanvas.height;"
"scopeCtx.fillStyle='#071018';"
"scopeCtx.fillRect(0,0,w,h);"
"scopeCtx.strokeStyle='#243746';"
"scopeCtx.lineWidth=1;"
"for(let x=0;x<=w;x+=100){"
"scopeCtx.beginPath();scopeCtx.moveTo(x,0);scopeCtx.lineTo(x,h);scopeCtx.stroke();"
"}"
"for(let y=0;y<=h;y+=45){"
"scopeCtx.beginPath();scopeCtx.moveTo(0,y);scopeCtx.lineTo(w,y);scopeCtx.stroke();"
"}"
"scopeCtx.fillStyle='#8fd3ff';"
"scopeCtx.font='14px monospace';"
"scopeCtx.textAlign='left';"
"scopeCtx.fillText('3.3 V',8,18);"
"scopeCtx.fillText('1.65 V',8,h/2-6);"
"scopeCtx.fillText('0 V',8,h-8);"
"if(values.length>1){"
"scopeCtx.strokeStyle='#00ff66';"
"scopeCtx.lineWidth=2;"
"scopeCtx.beginPath();"
"for(let i=0;i<values.length;i++){"
"const x=i*(w-1)/(values.length-1);"
"const y=h-1-(values[i]/4095)*(h-1);"
"if(i===0)scopeCtx.moveTo(x,y);else scopeCtx.lineTo(x,y);"
"}"
"scopeCtx.stroke();"
"}"
"const windowUs=values.length*command.step*1000000/command.rate;"
"const adcToVolt=value=>value*3.3/4095;"
"const frequency=command.frequency_millihz/1000;"
"scopeInfo.textContent='Fenster '+formatTimeUs(windowUs)+' | '+"
"'Min '+adcToVolt(command.min).toFixed(3)+' V | '+"
"'Max '+adcToVolt(command.max).toFixed(3)+' V | '+"
"'Mittel '+adcToVolt(command.average).toFixed(3)+' V | '+"
"'Frequenz '+(frequency>0?frequency.toFixed(1)+' Hz':'--');"
"break;}"

"case 'spectrum':{"
"if(command.sequence!==spectrumSequence){"
"spectrumSequence=command.sequence;spectrumChunks=new Array(command.parts).fill(null);"
"}"
"spectrumChunks[command.part]=command.samples||[];"
"if(spectrumChunks.every(part=>Array.isArray(part))){"
"drawSpectrum(spectrumChunks.flat(),command.rate);"
"spectrumChunks=new Array(command.parts).fill(null);"
"}"
"break;}"

"}"
"}"

"const events=new EventSource('/events');"

"events.onopen=()=>{"
"state.textContent='Mit Pico verbunden';"
"setSpectrumMax(spectrumMaxHz);"
"};"

"events.onerror=()=>{"
"state.textContent='Verbindung unterbrochen – neuer Versuch ...';"
"};"

"events.onmessage=(event)=>{"
"try{"
"executeCommand(JSON.parse(event.data));"
"}catch(error){"
"console.error(error);"
"}"
"};"


"function drawClockHand(angle,length,width,color){"
"clockCtx.save();"
"clockCtx.rotate(angle);"
"clockCtx.beginPath();"
"clockCtx.lineWidth=width;"
"clockCtx.lineCap='round';"
"clockCtx.strokeStyle=color;"
"clockCtx.moveTo(0,10);"
"clockCtx.lineTo(0,-length);"
"clockCtx.stroke();"
"clockCtx.restore();"
"}"

"function drawClock(){"
"const now=new Date();"
"const centerX=150;"
"const centerY=130;"
"const radius=105;"

"clockCtx.fillStyle='#071018';"
"clockCtx.fillRect(0,0,clockCanvas.width,clockCanvas.height);"

"clockCtx.save();"
"clockCtx.translate(centerX,centerY);"

"clockCtx.beginPath();"
"clockCtx.arc(0,0,radius,0,Math.PI*2);"
"clockCtx.fillStyle='#101c28';"
"clockCtx.fill();"
"clockCtx.lineWidth=4;"
"clockCtx.strokeStyle='#8fd3ff';"
"clockCtx.stroke();"

"for(let number=1;number<=12;number++){"
"const angle=number*Math.PI/6;"
"const x=Math.sin(angle)*(radius-22);"
"const y=-Math.cos(angle)*(radius-22);"
"clockCtx.fillStyle='#ffffff';"
"clockCtx.font='18px monospace';"
"clockCtx.textAlign='center';"
"clockCtx.textBaseline='middle';"
"clockCtx.fillText(number,x,y);"
"}"

"for(let mark=0;mark<60;mark++){"
"if(mark%5===0)continue;"
"const angle=mark*Math.PI/30;"
"const x1=Math.sin(angle)*(radius-10);"
"const y1=-Math.cos(angle)*(radius-10);"
"const x2=Math.sin(angle)*(radius-5);"
"const y2=-Math.cos(angle)*(radius-5);"
"clockCtx.beginPath();"
"clockCtx.strokeStyle='#657482';"
"clockCtx.lineWidth=1;"
"clockCtx.moveTo(x1,y1);"
"clockCtx.lineTo(x2,y2);"
"clockCtx.stroke();"
"}"

"const seconds=now.getSeconds()+now.getMilliseconds()/1000;"
"const minutes=now.getMinutes()+seconds/60;"
"const hours=(now.getHours()%12)+minutes/60;"

"drawClockHand(hours*Math.PI/6,50,7,'#ffffff');"
"drawClockHand(minutes*Math.PI/30,75,5,'#8fd3ff');"
"drawClockHand(seconds*Math.PI/30,87,2,'#ff5555');"

"clockCtx.beginPath();"
"clockCtx.arc(0,0,6,0,Math.PI*2);"
"clockCtx.fillStyle='#ffffff';"
"clockCtx.fill();"

"clockCtx.restore();"

"const timeText=now.toLocaleTimeString('de-DE');"
"const dateText=now.toLocaleDateString('de-DE',{"
"weekday:'long',"
"year:'numeric',"
"month:'long',"
"day:'numeric'"
"});"

"clockCtx.fillStyle='#00ff66';"
"clockCtx.font='54px monospace';"
"clockCtx.textAlign='left';"
"clockCtx.textBaseline='alphabetic';"
"clockCtx.fillText(timeText,330,120);"

"clockCtx.fillStyle='#ffffff';"
"clockCtx.font='22px sans-serif';"
"clockCtx.fillText(dateText,330,170);"
"}"

"drawClock();"
"setInterval(drawClock,250);"
"streamSpectrum();"
"</script>"
"</body>"
"</html>";

static int send_all(
    int socket_fd,
    const char *data,
    size_t length
)
{
    char tx_buffer[128];
    unsigned int buffer_retries = 0;

    while (length > 0) {
        size_t chunk_size = length;

        if (chunk_size > sizeof(tx_buffer)) {
            chunk_size = sizeof(tx_buffer);
        }

        /*
         * Wichtig beim Pico-WLAN:
         * Daten zunächst aus Flash in RAM kopieren.
         */
        memcpy(
            tx_buffer,
            data,
            chunk_size
        );

        int sent = zsock_send(
            socket_fd,
            tx_buffer,
            chunk_size,
            0
        );

        if (sent < 0) {
            /*
             * CYW43/WHD kann bei vielen FFT-Paketen kurzzeitig keinen
             * weiteren Netzwerkpuffer bereitstellen. Die SSE-Verbindung
             * deshalb bei ENOBUFS nicht sofort schließen.
             */
            if ((errno == ENOBUFS || errno == EAGAIN ||
                 errno == EWOULDBLOCK) && buffer_retries < 20u) {
                buffer_retries++;
                k_sleep(K_MSEC(10));
                continue;
            }

            printk(
                "WebDisplay: send failed, errno=%d\n",
                errno
            );
            return -errno;
        }

        if (sent == 0) {
            return -ECONNRESET;
        }

        data += sent;
        length -= (size_t)sent;
        buffer_retries = 0;

        /*
         * Der WHD-Treiber benötigt Zeit, um den kopierten RAM-Block
         * tatsächlich an den WLAN-Chip zu übergeben. Diese Pause gilt
         * auch für den letzten Block vor zsock_close().
         */
        k_sleep(K_MSEC(2));
    }

    return 0;
}

static void send_text_response(
    int client,
    const char *status,
    const char *text
)
{
    char header[192];
    size_t text_length = strlen(text);

    int header_length = snprintk(
        header,
        sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        status,
        (unsigned int)text_length
    );

    if (header_length > 0 &&
        (size_t)header_length < sizeof(header)) {
        (void)send_all(client, header, (size_t)header_length);
        (void)send_all(client, text, text_length);
    }
}

static void send_fft_response(int client)
{
    struct fft_binary_header binary_header;
    char http_header[192];

    k_mutex_lock(&scope_lock, K_FOREVER);

    uint32_t point_count = spectrum_point_count;

    if (point_count > 0 && point_count <= SPECTRUM_MAX_POINTS) {
        memcpy(spectrum_tx_samples,
               spectrum_samples,
               point_count * sizeof(spectrum_tx_samples[0]));
    }

    binary_header.magic = FFT_BINARY_MAGIC;
    binary_header.sequence = spectrum_generation;
    binary_header.sample_rate = spectrum_sample_rate;
    binary_header.sample_count = (uint16_t)point_count;
    binary_header.reserved = 0;

    k_mutex_unlock(&scope_lock);

    if (point_count == 0 || point_count > SPECTRUM_MAX_POINTS ||
        binary_header.sample_rate == 0) {
        send_text_response(client,
                           "503 Service Unavailable",
                           "Noch keine FFT-Daten");
        return;
    }

    size_t sample_bytes =
        point_count * sizeof(spectrum_tx_samples[0]);
    size_t content_length = sizeof(binary_header) + sample_bytes;

    int header_length = snprintk(
        http_header,
        sizeof(http_header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Cache-Control: no-store\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        (unsigned int)content_length
    );

    if (header_length <= 0 ||
        (size_t)header_length >= sizeof(http_header)) {
        return;
    }

    if (send_all(client,
                 http_header,
                 (size_t)header_length) < 0) {
        return;
    }

    if (send_all(client,
                 (const char *)&binary_header,
                 sizeof(binary_header)) < 0) {
        return;
    }

    (void)send_all(client,
                   (const char *)spectrum_tx_samples,
                   sample_bytes);
}


static int display_event(const char *json)
{
    char event[EVENT_SIZE];

    int length = snprintk(
        event,
        sizeof(event),
        "data: %s\n\n",
        json
    );

    if (length < 0 || length >= sizeof(event)) {
        return -EMSGSIZE;
    }

    int result = -ENOTCONN;

    k_mutex_lock(&event_lock, K_FOREVER);

    if (event_client >= 0) {
        result = send_all(
            event_client,
            event,
            (size_t)length
        );

        if (result < 0) {
            zsock_close(event_client);
            event_client = -1;
        }
    }

    k_mutex_unlock(&event_lock);

    return result;
}

/* Langsame Zeitbasen über mehrere DMA-Blöcke aufbauen. */
static void process_slow_scope(
    const uint16_t *samples,
    size_t sample_count,
    uint32_t sample_rate,
    uint32_t step
)
{
    if (slow_scope_step != step) {
        slow_scope_step = step;
        slow_scope_write = 0;
        slow_scope_count = 0;
        slow_scope_sum = 0;
        slow_scope_samples_in_sum = 0;
    }

    bool new_point = false;

    for (size_t index = 0; index < sample_count; index++) {
        slow_scope_sum += samples[index] & 0x0fff;
        slow_scope_samples_in_sum++;

        if (slow_scope_samples_in_sum == step) {
            slow_scope_ring[slow_scope_write] =
                (uint16_t)(slow_scope_sum / step);

            slow_scope_write =
                (slow_scope_write + 1u) % SCOPE_POINTS;

            if (slow_scope_count < SCOPE_POINTS) {
                slow_scope_count++;
            }

            slow_scope_sum = 0;
            slow_scope_samples_in_sum = 0;
            new_point = true;
        }
    }

    if (!new_point || slow_scope_count < SCOPE_POINTS) {
        return;
    }

    uint16_t ordered[SCOPE_POINTS];
    uint16_t minimum = 4095;
    uint16_t maximum = 0;
    uint32_t sum = 0;

    for (size_t point = 0; point < SCOPE_POINTS; point++) {
        size_t ring_index =
            (slow_scope_write + point) % SCOPE_POINTS;
        uint16_t value = slow_scope_ring[ring_index];

        ordered[point] = value;
        minimum = MIN(minimum, value);
        maximum = MAX(maximum, value);
        sum += value;
    }

    uint16_t average = (uint16_t)(sum / SCOPE_POINTS);
    uint16_t hysteresis = MAX((uint16_t)4,
                              (uint16_t)((maximum - minimum) / 20));
    bool armed = false;
    size_t first_crossing = 0;
    size_t last_crossing = 0;
    size_t crossings = 0;

    for (size_t point = 0; point < SCOPE_POINTS; point++) {
        uint16_t value = ordered[point];

        if (value + hysteresis < average) {
            armed = true;
        }

        if (armed && value > average + hysteresis) {
            if (crossings == 0) {
                first_crossing = point;
            }

            last_crossing = point;
            crossings++;
            armed = false;
        }
    }

    uint32_t frequency_millihz = 0;

    if (crossings >= 2 && last_crossing > first_crossing) {
        frequency_millihz = (uint32_t)(
            ((uint64_t)(crossings - 1u) * sample_rate * 1000u) /
            ((last_crossing - first_crossing) * step)
        );
    }

    k_mutex_lock(&scope_lock, K_FOREVER);

    memcpy(scope_samples, ordered, sizeof(ordered));
    scope_minimum = minimum;
    scope_maximum = maximum;
    scope_average = average;
    scope_frequency_millihz = frequency_millihz;
    scope_sample_rate = sample_rate;
    scope_step_used = step;
    scope_generation++;

    k_mutex_unlock(&scope_lock);
}

/*
 * FFT-Daten bereitstellen.
 *
 * Im schnellen Modus wird ein kompletter 1024er ADC-Block verwendet.
 * Der 20-kHz-Modus mittelt jeweils 10 Rohwerte und sammelt 2048 Werte.
 * Daraus folgen 40000 Samples/s und 40000 / 2048 = 19,53125 Hz/Bin.
 */
static void process_spectrum(
    const uint16_t *samples,
    size_t sample_count,
    uint32_t sample_rate
)
{
    uint32_t decimation =
        (uint32_t)atomic_get(&spectrum_requested_decimation);

    if (decimation != spectrum_active_decimation) {
        spectrum_active_decimation = decimation;
        spectrum_hires_count = 0;
        spectrum_decimation_sum = 0;
        spectrum_decimation_count = 0;
    }

    if (decimation == 1u) {
        if (sample_count < SPECTRUM_FAST_POINTS) {
            return;
        }

        k_mutex_lock(&scope_lock, K_FOREVER);

        for (size_t index = 0; index < SPECTRUM_FAST_POINTS; index++) {
            spectrum_samples[index] = samples[index] & 0x0fff;
        }

        spectrum_sample_rate = sample_rate;
        spectrum_point_count = SPECTRUM_FAST_POINTS;
        spectrum_generation++;

        k_mutex_unlock(&scope_lock);
        return;
    }

    for (size_t index = 0; index < sample_count; index++) {
        spectrum_decimation_sum += samples[index] & 0x0fff;
        spectrum_decimation_count++;

        if (spectrum_decimation_count == decimation) {
            spectrum_hires_build[spectrum_hires_count++] =
                (uint16_t)(spectrum_decimation_sum / decimation);

            spectrum_decimation_sum = 0;
            spectrum_decimation_count = 0;

            if (spectrum_hires_count == SPECTRUM_HIRES_POINTS) {
                k_mutex_lock(&scope_lock, K_FOREVER);

                memcpy(spectrum_samples,
                       spectrum_hires_build,
                       sizeof(spectrum_hires_build));

                spectrum_sample_rate = sample_rate / decimation;
                spectrum_point_count = SPECTRUM_HIRES_POINTS;
                spectrum_generation++;

                k_mutex_unlock(&scope_lock);
                spectrum_hires_count = 0;
            }
        }
    }
}

/* ADC-Block auswerten, auf eine steigende Flanke triggern und verkleinern. */
static void scope_adc_block(
    const uint16_t *samples,
    size_t sample_count,
    uint32_t sample_rate
)
{
    static uint32_t trigger_misses;
    uint32_t step;

    step = (uint32_t)atomic_get(&scope_requested_step);

    if (samples == NULL || sample_count == 0) {
        return;
    }

    /* FFT erhält unabhängig von der Oszilloskop-Zeitbasis ADC-Daten. */
    process_spectrum(samples, sample_count, sample_rate);

    if (step > SCOPE_DEFAULT_STEP) {
        process_slow_scope(samples, sample_count, sample_rate, step);
        return;
    }

    slow_scope_step = 0;

    if (sample_count < ((SCOPE_POINTS - 1u) * step + 1u)) {
        return;
    }

    uint16_t minimum = 4095;
    uint16_t maximum = 0;
    uint32_t sum = 0;

    for (size_t index = 0; index < sample_count; index++) {
        uint16_t value = samples[index] & 0x0fff;
        minimum = MIN(minimum, value);
        maximum = MAX(maximum, value);
        sum += value;
    }

    uint16_t average = (uint16_t)(sum / sample_count);
    uint16_t hysteresis = MAX((uint16_t)8,
                              (uint16_t)((maximum - minimum) / 20));
    bool armed = false;
    size_t first_crossing = 0;
    size_t last_crossing = 0;
    size_t crossing_count = 0;
    size_t trigger_crossing = 0;
    bool trigger_found = false;
    size_t pretrigger = SCOPE_TRIGGER_POINT * step;
    size_t posttrigger =
        (SCOPE_POINTS - 1u - SCOPE_TRIGGER_POINT) * step;

    for (size_t index = 1; index < sample_count; index++) {
        uint16_t value = samples[index] & 0x0fff;

        if (value + hysteresis < average) {
            armed = true;
        }

        if (armed && value > average + hysteresis) {
            if (crossing_count == 0) {
                first_crossing = index;
            }

            last_crossing = index;
            crossing_count++;

            /*
             * Nur eine Flanke verwenden, bei der genügend Samples
             * vor und nach dem Triggerpunkt vorhanden sind. Dadurch
             * liegt die Flanke immer bei Punkt 25 des Scope-Bildes.
             */
            if (!trigger_found &&
                index >= pretrigger &&
                index + posttrigger < sample_count) {
                trigger_crossing = index;
                trigger_found = true;
            }

            armed = false;
        }
    }

    uint32_t frequency_millihz = 0;

    if (crossing_count >= 2 && last_crossing > first_crossing) {
        frequency_millihz = (uint32_t)(
            ((uint64_t)(crossing_count - 1) * sample_rate * 1000u) /
            (last_crossing - first_crossing)
        );
    }

    size_t start = 0;

    if (trigger_found) {
        trigger_misses = 0;
        start = trigger_crossing - pretrigger;
    } else if ((maximum - minimum) > 32u) {
        /*
         * Ein Signal ist vorhanden, aber in diesem Block liegt keine
         * vollständig darstellbare Triggerflanke. Den alten, stabilen
         * Bildschirm behalten. Nach längerer Zeit einmal freilaufend
         * aktualisieren, damit sehr langsame Signale sichtbar bleiben.
         */
        trigger_misses++;

        if (trigger_misses < 200u) {
            return;
        }

        trigger_misses = 0;
    }

    size_t required = (SCOPE_POINTS - 1u) * step;

    if (start + required >= sample_count) {
        start = sample_count > required
              ? sample_count - required - 1u
              : 0;
    }

    k_mutex_lock(&scope_lock, K_FOREVER);

    for (size_t point = 0; point < SCOPE_POINTS; point++) {
        size_t index = start + point * step;
        scope_samples[point] = samples[index] & 0x0fff;
    }

    scope_minimum = minimum;
    scope_maximum = maximum;
    scope_average = average;
    scope_frequency_millihz = frequency_millihz;
    scope_sample_rate = sample_rate;
    scope_step_used = step;
    scope_generation++;

    k_mutex_unlock(&scope_lock);
}

/*
 * Maximal etwa 20 Bilder pro Sekunde zum Browser senden.
 * Falls der ADC schneller ist, wird immer der jüngste Block angezeigt.
 * Vorläufig deaktiviert: FFT verwendet eine eigene binaere HTTP-Abfrage.
 */
#if 0
static void scope_thread(
    void *arg1,
    void *arg2,
    void *arg3
)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    uint32_t last_generation = 0;
    uint16_t local_samples[SCOPE_POINTS];

    while (true) {
        k_sleep(K_MSEC(50));

        uint32_t generation;
        uint32_t sample_rate;
        uint16_t minimum;
        uint16_t maximum;
        uint16_t average;
        uint32_t frequency_millihz;
        uint32_t step;

        k_mutex_lock(&scope_lock, K_FOREVER);

        generation = scope_generation;
        sample_rate = scope_sample_rate;
        minimum = scope_minimum;
        maximum = scope_maximum;
        average = scope_average;
        frequency_millihz = scope_frequency_millihz;
        step = scope_step_used;

        if (generation != last_generation) {
            memcpy(local_samples,
                   scope_samples,
                   sizeof(local_samples));
        }

        k_mutex_unlock(&scope_lock);

        if (generation == last_generation || sample_rate == 0) {
            continue;
        }

        last_generation = generation;

        char json[EVENT_SIZE - 16];
        size_t used = 0;

        int written = snprintk(
            json,
            sizeof(json),
            "{\"cmd\":\"scope\",\"rate\":%u,\"step\":%u,"
            "\"min\":%u,\"max\":%u,\"average\":%u,"
            "\"frequency_millihz\":%u,\"samples\":[",
            sample_rate,
            step,
            minimum,
            maximum,
            average,
            frequency_millihz
        );

        if (written < 0 || (size_t)written >= sizeof(json)) {
            continue;
        }

        used = (size_t)written;

        for (size_t index = 0; index < SCOPE_POINTS; index++) {
            written = snprintk(
                json + used,
                sizeof(json) - used,
                "%s%u",
                index == 0 ? "" : ",",
                local_samples[index]
            );

            if (written < 0 ||
                (size_t)written >= sizeof(json) - used) {
                used = 0;
                break;
            }

            used += (size_t)written;
        }

        if (used == 0 || used + 3 >= sizeof(json)) {
            continue;
        }

        json[used++] = ']';
        json[used++] = '}';
        json[used] = '\0';

        (void)display_event(json);
    }
}

K_THREAD_DEFINE(
    scope_thread_id,
    7168,
    scope_thread,
    NULL,
    NULL,
    NULL,
    14,
    0,
    0
);
#endif

/* FFT-Rohdaten mit geringer Bildrate an den Browser übertragen. */
#if 0
static void spectrum_thread(
    void *arg1,
    void *arg2,
    void *arg3
)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    uint32_t last_generation = 0;

    while (true) {
        k_sleep(K_MSEC(200));

        uint32_t generation;
        uint32_t sample_rate;
        uint32_t point_count;

        k_mutex_lock(&scope_lock, K_FOREVER);

        generation = spectrum_generation;
        sample_rate = spectrum_sample_rate;
        point_count = spectrum_point_count;

        if (generation != last_generation) {
            memcpy(spectrum_tx_samples,
                   spectrum_samples,
                   point_count * sizeof(spectrum_tx_samples[0]));
        }

        k_mutex_unlock(&scope_lock);

        if (generation == last_generation || sample_rate == 0 ||
            point_count == 0 || point_count > SPECTRUM_MAX_POINTS) {
            continue;
        }

        last_generation = generation;
        uint32_t chunks = point_count / SPECTRUM_CHUNK_POINTS;

        for (uint32_t part = 0; part < chunks; part++) {
            char json[EVENT_SIZE - 16];
            size_t used = 0;

            int written = snprintk(
                json,
                sizeof(json),
                "{\"cmd\":\"spectrum\",\"rate\":%u,"
                "\"sequence\":%u,\"part\":%u,\"parts\":%u,"
                "\"samples\":[",
                sample_rate,
                generation,
                part,
                chunks
            );

            if (written < 0 || (size_t)written >= sizeof(json)) {
                break;
            }

            used = (size_t)written;

            size_t first = part * SPECTRUM_CHUNK_POINTS;
            size_t end = first + SPECTRUM_CHUNK_POINTS;

            for (size_t index = first; index < end; index++) {
                written = snprintk(
                    json + used,
                    sizeof(json) - used,
                    "%s%u",
                    index == first ? "" : ",",
                    spectrum_tx_samples[index]
                );

                if (written < 0 ||
                    (size_t)written >= sizeof(json) - used) {
                    used = 0;
                    break;
                }

                used += (size_t)written;
            }

            if (used == 0 || used + 3 >= sizeof(json)) {
                break;
            }

            json[used++] = ']';
            json[used++] = '}';
            json[used] = '\0';

            if (display_event(json) < 0) {
                break;
            }

            /* WLAN-Puffer zwischen zwei FFT-Teilpaketen abarbeiten lassen. */
            k_sleep(K_MSEC(10));
        }
    }
}

K_THREAD_DEFINE(
    spectrum_thread_id,
    9216,
    spectrum_thread,
    NULL,
    NULL,
    NULL,
    15,
    0,
    0
);
#endif

static void serve_connection(int client)
{
    char request[RX_SIZE];

    /*
     * Firefox kann vorsorglich TCP-Verbindungen oeffnen, ohne sofort einen
     * HTTP-Request zu senden. Ohne Timeout wuerde unser einzelner HTTP-Thread
     * dauerhaft in recv() warten und die Accept-Warteschlange volllaufen.
     */
    struct timeval receive_timeout = {
        .tv_sec = 0,
        .tv_usec = HTTP_RECEIVE_TIMEOUT_MS * 1000,
    };

    (void)zsock_setsockopt(
        client,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &receive_timeout,
        sizeof(receive_timeout)
    );

    int received = zsock_recv(
        client,
        request,
        sizeof(request) - 1,
        0
    );

    if (received <= 0) {
        zsock_close(client);
        return;
    }

    request[received] = '\0';

    /*
     * Socket an den FFT-Stream-Thread uebergeben. Die Scope-Zeitbasis wird
     * beim Aufbau derselben dauerhaften Verbindung mitgeliefert. Dadurch
     * braucht Firefox keinen dritten HTTP-Socket fuer einen Steuerbefehl.
     */
    if (strncmp(request, "GET /fft-stream", 15) == 0 &&
        (request[15] == ' ' || request[15] == '?')) {
        const char *parameter = strstr(request, "?scope-step=");

        if (parameter != NULL) {
            unsigned long step = strtoul(parameter + 12, NULL, 10);
            bool valid =
                step == 1 || step == 2 || step == 4 || step == 8 ||
                step == 80 || step == 800 || step == 8000 ||
                step == 80000;

            if (!valid) {
                send_text_response(client,
                                   "400 Bad Request",
                                   "ungueltige Scope-Zeitbasis");
                zsock_close(client);
                return;
            }

            atomic_set(&scope_requested_step, (atomic_val_t)step);
        }

        if (k_msgq_put(&fft_client_queue, &client, K_NO_WAIT) != 0) {
            send_text_response(client,
                               "503 Service Unavailable",
                               "FFT-Stream ist belegt");
            zsock_close(client);
        }

        return;
    }

    /* Scope-Pakete im gemeinsamen FFT-Binaerstream ein-/ausschalten. */
    if (strncmp(request, "GET /scope-enable?value=", 24) == 0) {
        char value = request[24];

        if ((value != '0' && value != '1') ||
            (request[25] != ' ' && request[25] != '&')) {
            send_text_response(client,
                               "400 Bad Request",
                               "value muss 0 oder 1 sein");
        } else {
            static const char ok[] =
                "HTTP/1.1 204 No Content\r\n"
                "Cache-Control: no-store\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";

            atomic_set(&scope_stream_enabled, value == '1');
            (void)send_all(client, ok, strlen(ok));
        }

        zsock_close(client);
        return;
    }

    /* FFT als kompakter binaerer Snapshot mit Browser-Flusskontrolle. */
    if (strncmp(request, "GET /fft ", 9) == 0) {
        send_fft_response(client);
        zsock_close(client);
        return;
    }

    /*
     * Permanente Ereignisverbindung zum Browser.
     */
    if (strncmp(request, "GET /events ", 12) == 0) {
        static const char event_header[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n"
            ": WebDisplay connected\n\n";

        if (send_all(
                client,
                event_header,
                strlen(event_header)
            ) < 0) {
            zsock_close(client);
            return;
        }

        /* Ein langsamer Browser darf den Scope-Thread nicht festhalten. */
        struct timeval send_timeout = {
            .tv_sec = 0,
            .tv_usec = 250000,
        };

        (void)zsock_setsockopt(
            client,
            SOL_SOCKET,
            SO_SNDTIMEO,
            &send_timeout,
            sizeof(send_timeout)
        );

        k_mutex_lock(&event_lock, K_FOREVER);

        if (event_client >= 0) {
            zsock_close(event_client);
        }

        event_client = client;

        k_mutex_unlock(&event_lock);

        printk("WebDisplay: browser connected\n");
        return;
    }

    /* Zeitbasis aus dem Browser setzen. */
    if (strncmp(request, "GET /scope?step=", 16) == 0) {
        unsigned long step = strtoul(request + 16, NULL, 10);
        bool valid =
            step == 1 || step == 2 || step == 4 || step == 8 ||
            step == 80 || step == 800 || step == 8000 ||
            step == 80000;

        if (valid) {
            static const char ok[] =
                "HTTP/1.1 204 No Content\r\n"
                "Cache-Control: no-store\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";

            atomic_set(&scope_requested_step, (atomic_val_t)step);

            (void)send_all(client, ok, strlen(ok));
        } else {
            static const char bad_request[] =
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";

            (void)send_all(client,
                           bad_request,
                           strlen(bad_request));
        }

        zsock_close(client);
        return;
    }

    /* 20 kHz: 4096 Punkte bei 40 kSamples/s; andere Bereiche: schneller Modus. */
    if (strncmp(request, "GET /spectrum?max=", 18) == 0) {
        unsigned long maximum = strtoul(request + 18, NULL, 10);
        bool valid =
            maximum == 20000 || maximum == 50000 ||
            maximum == 100000 || maximum == 200000;

        if (valid) {
            uint32_t decimation = maximum == 20000 ?
                SPECTRUM_DECIMATION_20KHZ : 1u;

            atomic_set(&spectrum_requested_decimation,
                       (atomic_val_t)decimation);

            static const char ok[] =
                "HTTP/1.1 204 No Content\r\n"
                "Cache-Control: no-store\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";

            (void)send_all(client, ok, strlen(ok));
        } else {
            static const char bad_request[] =
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";

            (void)send_all(client,
                           bad_request,
                           strlen(bad_request));
        }

        zsock_close(client);
        return;
    }

    if (strncmp(request, "GET /adc/start ", 15) == 0) {
        int result = adc_dma_start(1000000u);

        if (result == 0) {
            send_text_response(client, "200 OK", "ADC laeuft");
        } else if (result == -EBUSY) {
            send_text_response(client, "409 Conflict", "ADC laeuft bereits");
        } else {
            send_text_response(client, "500 Internal Server Error", "ADC Startfehler");
        }

        zsock_close(client);
        return;
    }

    if (strncmp(request, "GET /adc/stop ", 14) == 0) {
        adc_dma_stop();
        send_text_response(client, "200 OK", "ADC gestoppt");
        zsock_close(client);
        return;
    }

    if (strncmp(request, "GET /adc/single ", 16) == 0) {
        int result = adc_dma_start(1u);

        if (result == 0) {
            send_text_response(client, "200 OK", "Single aufgenommen");
        } else if (result == -EBUSY) {
            send_text_response(client, "409 Conflict", "Zuerst ADC stoppen");
        } else {
            send_text_response(client, "500 Internal Server Error", "Single Fehler");
        }

        zsock_close(client);
        return;
    }

    /*
     * Hauptseite an Firefox senden.
     */
    if (strncmp(request, "GET / ", 6) == 0) {
        char header[192];

        int header_length = snprintk(
            header,
            sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Cache-Control: no-store\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n"
            "\r\n",
            (unsigned int)strlen(index_html)
        );

        (void)send_all(
            client,
            header,
            (size_t)header_length
        );

        (void)send_all(
            client,
            index_html,
            strlen(index_html)
        );
    } else {
        static const char not_found[] =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n";

        (void)send_all(
            client,
            not_found,
            strlen(not_found)
        );
    }

    zsock_close(client);
}

static void http_thread(
    void *arg1,
    void *arg2,
    void *arg3
)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(HTTP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    int server = zsock_socket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP
    );

    if (server < 0) {
        printk(
            "WebDisplay: socket failed, errno=%d\n",
            errno
        );
        return;
    }

    if (zsock_bind(
            server,
            (struct sockaddr *)&address,
            sizeof(address)
        ) < 0) {
        printk(
            "WebDisplay: bind failed, errno=%d\n",
            errno
        );
        zsock_close(server);
        return;
    }

    if (zsock_listen(server, HTTP_LISTEN_BACKLOG) < 0) {
        printk(
            "WebDisplay: listen failed, errno=%d\n",
            errno
        );
        zsock_close(server);
        return;
    }

    printk(
        "WebDisplay: HTTP server on port %d\n",
        HTTP_PORT
    );

    while (true) {
        int client = zsock_accept(
            server,
            NULL,
            NULL
        );

        if (client >= 0) {
            serve_connection(client);
        }
    }
}

K_THREAD_DEFINE(
    http_thread_id,
    4096,
    http_thread,
    NULL,
    NULL,
    NULL,
    15,
    0,
    0
);

/* Alte Variante mit separatem Port 8081, nur noch als Referenz. */
#if 0
static void fft_stream_thread(
    void *arg1,
    void *arg2,
    void *arg3
)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(FFT_STREAM_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    int server = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (server < 0) {
        printk("FFT stream: socket failed, errno=%d\n", errno);
        return;
    }

    if (zsock_bind(server,
                   (struct sockaddr *)&address,
                   sizeof(address)) < 0) {
        printk("FFT stream: bind failed, errno=%d\n", errno);
        zsock_close(server);
        return;
    }

    if (zsock_listen(server, 1) < 0) {
        printk("FFT stream: listen failed, errno=%d\n", errno);
        zsock_close(server);
        return;
    }

    printk("FFT stream: binary server on port %d\n", FFT_STREAM_PORT);

    while (true) {
        int client = zsock_accept(server, NULL, NULL);

        if (client < 0) {
            k_sleep(K_MSEC(20));
            continue;
        }

        char request[128];
        int received = zsock_recv(client,
                                  request,
                                  sizeof(request) - 1,
                                  0);

        if (received <= 0) {
            zsock_close(client);
            continue;
        }

        static const char stream_header[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Cache-Control: no-store\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";

        if (send_all(client,
                     stream_header,
                     strlen(stream_header)) < 0) {
            zsock_close(client);
            continue;
        }

        printk("FFT stream: browser connected\n");
        uint32_t last_generation = 0;
        uint32_t last_scope_generation = 0;

        while (true) {
            struct scope_binary_header binary_header;
            struct scope_binary_header scope_header;
            uint32_t point_count;

            k_mutex_lock(&scope_lock, K_FOREVER);

            point_count = spectrum_point_count;
            binary_header.magic = FFT_BINARY_MAGIC;
            binary_header.sequence = spectrum_generation;
            binary_header.sample_rate = spectrum_sample_rate;
            binary_header.sample_count = (uint16_t)point_count;
            binary_header.reserved0 = 0;
            binary_header.step = 0;
            binary_header.frequency_millihz = 0;
            binary_header.minimum = 0;
            binary_header.maximum = 0;
            binary_header.average = 0;
            binary_header.reserved1 = 0;

            bool new_frame =
                point_count > 0 &&
                point_count <= SPECTRUM_MAX_POINTS &&
                binary_header.sample_rate > 0 &&
                binary_header.sequence != last_generation;

            if (new_frame) {
                memcpy(spectrum_stream_samples,
                       spectrum_samples,
                       point_count * sizeof(spectrum_stream_samples[0]));
            }

            scope_header.magic = SCOPE_BINARY_MAGIC;
            scope_header.sequence = scope_generation;
            scope_header.sample_rate = scope_sample_rate;
            scope_header.sample_count = SCOPE_POINTS;
            scope_header.reserved0 = 0;
            scope_header.step = scope_step_used;
            scope_header.frequency_millihz = scope_frequency_millihz;
            scope_header.minimum = scope_minimum;
            scope_header.maximum = scope_maximum;
            scope_header.average = scope_average;
            scope_header.reserved1 = 0;

            bool new_scope_frame =
                atomic_get(&scope_stream_enabled) != 0 &&
                scope_header.sample_rate > 0 &&
                scope_header.sequence != last_scope_generation;

            if (new_scope_frame) {
                memcpy(scope_stream_samples,
                       scope_samples,
                       sizeof(scope_stream_samples));
            }

            k_mutex_unlock(&scope_lock);

            if (!new_frame) {
                k_sleep(K_MSEC(2));
                continue;
            }

            size_t frame_bytes =
                sizeof(binary_header) +
                point_count * sizeof(spectrum_stream_samples[0]);
            char chunk_header[16];
            int chunk_header_length = snprintk(
                chunk_header,
                sizeof(chunk_header),
                "%x\r\n",
                (unsigned int)frame_bytes
            );

            if (chunk_header_length <= 0 ||
                (size_t)chunk_header_length >= sizeof(chunk_header)) {
                break;
            }

            if (send_all(client,
                         chunk_header,
                         (size_t)chunk_header_length) < 0) {
                break;
            }

            if (send_all(client,
                         (const char *)&binary_header,
                         sizeof(binary_header)) < 0) {
                break;
            }

            if (send_all(client,
                         (const char *)spectrum_stream_samples,
                         point_count * sizeof(spectrum_stream_samples[0])) < 0) {
                break;
            }

            if (send_all(client, "\r\n", 2) < 0) {
                break;
            }

            last_generation = binary_header.sequence;
            k_sleep(K_MSEC(2));
        }

        zsock_close(client);
        printk("FFT stream: browser disconnected\n");
    }
}

K_THREAD_DEFINE(
    fft_stream_thread_id,
    4096,
    fft_stream_thread,
    NULL,
    NULL,
    NULL,
    15,
    0,
    0
);
#endif

/* Dauerhafter binaerer FFT-Stream ueber denselben Port wie die Webseite. */
static void fft_stream_thread(
    void *arg1,
    void *arg2,
    void *arg3
)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    while (true) {
        int client;

        k_msgq_get(&fft_client_queue, &client, K_FOREVER);

        static const char stream_header[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Cache-Control: no-store\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";

        if (send_all(client,
                     stream_header,
                     strlen(stream_header)) < 0) {
            zsock_close(client);
            continue;
        }

        printk("FFT stream: browser connected\n");
        uint32_t last_generation = 0;
        uint32_t last_scope_generation = 0;

        while (true) {
            struct scope_binary_header binary_header;
            struct scope_binary_header scope_header;
            uint32_t point_count;

            k_mutex_lock(&scope_lock, K_FOREVER);

            point_count = spectrum_point_count;
            binary_header.magic = FFT_BINARY_MAGIC;
            binary_header.sequence = spectrum_generation;
            binary_header.sample_rate = spectrum_sample_rate;
            binary_header.sample_count = (uint16_t)point_count;
            binary_header.reserved0 = 0;
            binary_header.step = 0;
            binary_header.frequency_millihz = 0;
            binary_header.minimum = 0;
            binary_header.maximum = 0;
            binary_header.average = 0;
            binary_header.reserved1 = 0;

            bool new_frame =
                point_count > 0 &&
                point_count <= SPECTRUM_MAX_POINTS &&
                binary_header.sample_rate > 0 &&
                binary_header.sequence != last_generation;

            if (new_frame) {
                memcpy(spectrum_stream_samples,
                       spectrum_samples,
                       point_count * sizeof(spectrum_stream_samples[0]));
            }

            scope_header.magic = SCOPE_BINARY_MAGIC;
            scope_header.sequence = scope_generation;
            scope_header.sample_rate = scope_sample_rate;
            scope_header.sample_count = SCOPE_POINTS;
            scope_header.reserved0 = 0;
            scope_header.step = scope_step_used;
            scope_header.frequency_millihz = scope_frequency_millihz;
            scope_header.minimum = scope_minimum;
            scope_header.maximum = scope_maximum;
            scope_header.average = scope_average;
            scope_header.reserved1 = 0;

            bool new_scope_frame =
                atomic_get(&scope_stream_enabled) != 0 &&
                scope_header.sample_rate > 0 &&
                scope_header.sequence != last_scope_generation;

            if (new_scope_frame) {
                memcpy(scope_stream_samples,
                       scope_samples,
                       sizeof(scope_stream_samples));
            }

            k_mutex_unlock(&scope_lock);

            if (!new_frame) {
                k_sleep(K_MSEC(2));
                continue;
            }

            size_t frame_bytes =
                sizeof(binary_header) +
                point_count * sizeof(spectrum_stream_samples[0]);
            char chunk_header[16];
            int chunk_header_length = snprintk(
                chunk_header,
                sizeof(chunk_header),
                "%x\r\n",
                (unsigned int)frame_bytes
            );

            if (chunk_header_length <= 0 ||
                (size_t)chunk_header_length >= sizeof(chunk_header)) {
                break;
            }

            if (send_all(client,
                         chunk_header,
                         (size_t)chunk_header_length) < 0 ||
                send_all(client,
                         (const char *)&binary_header,
                         sizeof(binary_header)) < 0 ||
                send_all(client,
                         (const char *)spectrum_stream_samples,
                         point_count * sizeof(spectrum_stream_samples[0])) < 0 ||
                send_all(client, "\r\n", 2) < 0) {
                break;
            }

            last_generation = binary_header.sequence;

            if (new_scope_frame) {
                frame_bytes = sizeof(scope_header) + sizeof(scope_stream_samples);
                chunk_header_length = snprintk(
                    chunk_header,
                    sizeof(chunk_header),
                    "%x\r\n",
                    (unsigned int)frame_bytes
                );

                if (chunk_header_length <= 0 ||
                    (size_t)chunk_header_length >= sizeof(chunk_header) ||
                    send_all(client,
                             chunk_header,
                             (size_t)chunk_header_length) < 0 ||
                    send_all(client,
                             (const char *)&scope_header,
                             sizeof(scope_header)) < 0 ||
                    send_all(client,
                             (const char *)scope_stream_samples,
                             sizeof(scope_stream_samples)) < 0 ||
                    send_all(client, "\r\n", 2) < 0) {
                    break;
                }

                last_scope_generation = scope_header.sequence;
            }

            k_sleep(K_MSEC(10));
        }

        zsock_close(client);
        printk("FFT stream: browser disconnected\n");
    }
}

K_THREAD_DEFINE(
    fft_stream_thread_id,
    4096,
    fft_stream_thread,
    NULL,
    NULL,
    NULL,
    15,
    0,
    0
);

#if 0
/* Alte separate Scope-Verbindung: durch Multiplex-Pakete ersetzt. */
static void scope_stream_thread(
    void *arg1,
    void *arg2,
    void *arg3
)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    while (true) {
        int client;

        k_msgq_get(&scope_client_queue, &client, K_FOREVER);

        static const char stream_header[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Cache-Control: no-store\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";

        if (send_all(client,
                     stream_header,
                     strlen(stream_header)) < 0) {
            zsock_close(client);
            continue;
        }

        printk("Scope stream: browser connected\n");
        uint32_t last_generation = 0;

        while (true) {
            struct scope_binary_header binary_header;

            k_mutex_lock(&scope_lock, K_FOREVER);

            binary_header.magic = SCOPE_BINARY_MAGIC;
            binary_header.sequence = scope_generation;
            binary_header.sample_rate = scope_sample_rate;
            binary_header.sample_count = SCOPE_POINTS;
            binary_header.reserved0 = 0;
            binary_header.step = scope_step_used;
            binary_header.frequency_millihz = scope_frequency_millihz;
            binary_header.minimum = scope_minimum;
            binary_header.maximum = scope_maximum;
            binary_header.average = scope_average;
            binary_header.reserved1 = 0;

            bool new_frame =
                binary_header.sample_rate > 0 &&
                binary_header.sequence != last_generation;

            if (new_frame) {
                memcpy(scope_stream_samples,
                       scope_samples,
                       sizeof(scope_stream_samples));
            }

            k_mutex_unlock(&scope_lock);

            if (!new_frame) {
                k_sleep(K_MSEC(2));
                continue;
            }

            size_t frame_bytes =
                sizeof(binary_header) + sizeof(scope_stream_samples);
            char chunk_header[16];
            int chunk_header_length = snprintk(
                chunk_header,
                sizeof(chunk_header),
                "%x\r\n",
                (unsigned int)frame_bytes
            );

            if (chunk_header_length <= 0 ||
                (size_t)chunk_header_length >= sizeof(chunk_header)) {
                break;
            }

            if (send_all(client,
                         chunk_header,
                         (size_t)chunk_header_length) < 0 ||
                send_all(client,
                         (const char *)&binary_header,
                         sizeof(binary_header)) < 0 ||
                send_all(client,
                         (const char *)scope_stream_samples,
                         sizeof(scope_stream_samples)) < 0 ||
                send_all(client, "\r\n", 2) < 0) {
                break;
            }

            last_generation = binary_header.sequence;
            k_sleep(K_MSEC(50));
        }

        zsock_close(client);
        printk("Scope stream: browser disconnected\n");
    }
}

K_THREAD_DEFINE(
    scope_stream_thread_id,
    3072,
    scope_stream_thread,
    NULL,
    NULL,
    NULL,
    15,
    0,
    0
);
#endif

/*
 * display clear [Farbe]
 */
static int command_clear(
    const struct shell *shell,
    size_t argc,
    char **argv
)
{
    char json[96];

    const char *color =
        argc > 1 ? argv[1] : "#000000";

    snprintk(
        json,
        sizeof(json),
        "{\"cmd\":\"clear\",\"color\":\"%s\"}",
        color
    );

    int result = display_event(json);

    if (result == 0) {
        shell_print(shell, "clear sent");
        return 0;
    }

    shell_error(shell, "no browser connected");
    return -ENOEXEC;
}

/*
 * display line x1 y1 x2 y2
 */
static int command_line(
    const struct shell *shell,
    size_t argc,
    char **argv
)
{
    if (argc != 5) {
        shell_error(
            shell,
            "usage: display line x1 y1 x2 y2"
        );
        return -EINVAL;
    }

    char json[180];

    snprintk(
        json,
        sizeof(json),
        "{"
        "\"cmd\":\"line\","
        "\"x1\":%d,"
        "\"y1\":%d,"
        "\"x2\":%d,"
        "\"y2\":%d,"
        "\"color\":\"#00ff66\","
        "\"width\":2"
        "}",
        atoi(argv[1]),
        atoi(argv[2]),
        atoi(argv[3]),
        atoi(argv[4])
    );

    if (display_event(json) < 0) {
        shell_error(shell, "no browser connected");
        return -ENOEXEC;
    }

    return 0;
}

/*
 * display text x y Nachricht
 */
static int command_text(
    const struct shell *shell,
    size_t argc,
    char **argv
)
{
    if (argc < 4) {
        shell_error(
            shell,
            "usage: display text x y message"
        );
        return -EINVAL;
    }

char message[160] = {0};
size_t used = 0;

for (size_t index = 3; index < argc; index++) {
    int written = snprintk(
        message + used,
        sizeof(message) - used,
        "%s%s",
        index > 3 ? " " : "",
        argv[index]
    );

    if (written < 0) {
        break;
    }

    if ((size_t)written >= sizeof(message) - used) {
        used = sizeof(message) - 1;
        break;
    }

    used += (size_t)written;
}



    char json[EVENT_SIZE - 16];

    snprintk(
        json,
        sizeof(json),
        "{"
        "\"cmd\":\"text\","
        "\"x\":%d,"
        "\"y\":%d,"
        "\"size\":32,"
        "\"color\":\"#ffffff\","
        "\"text\":\"%s\""
        "}",
        atoi(argv[1]),
        atoi(argv[2]),
        message
    );

    if (display_event(json) < 0) {
        shell_error(shell, "no browser connected");
        return -ENOEXEC;
    }

    return 0;
}

/*
 * Fertiges Hello-World-Testbild.
 */
static int command_hello(
    const struct shell *shell,
    size_t argc,
    char **argv
)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    if (display_event(
            "{"
            "\"cmd\":\"clear\","
            "\"color\":\"#071018\""
            "}"
        ) < 0) {
        shell_error(shell, "no browser connected");
        return -ENOEXEC;
    }

    (void)display_event(
        "{"
        "\"cmd\":\"line\","
        "\"x1\":40,"
        "\"y1\":100,"
        "\"x2\":760,"
        "\"y2\":100,"
        "\"color\":\"#00ff66\","
        "\"width\":3"
        "}"
    );

    (void)display_event(
        "{"
        "\"cmd\":\"text\","
        "\"x\":70,"
        "\"y\":230,"
        "\"size\":48,"
        "\"color\":\"#8fd3ff\","
        "\"text\":\"Hello World from Zephyr\""
        "}"
    );

    (void)display_event(
        "{"
        "\"cmd\":\"line\","
        "\"x1\":40,"
        "\"y1\":300,"
        "\"x2\":760,"
        "\"y2\":300,"
        "\"color\":\"#00ff66\","
        "\"width\":3"
        "}"
    );

    shell_print(shell, "hello display sent");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    display_commands,

    SHELL_CMD(
        clear,
        NULL,
        "Clear: display clear [#rrggbb]",
        command_clear
    ),

    SHELL_CMD(
        line,
        NULL,
        "Line: display line x1 y1 x2 y2",
        command_line
    ),

    SHELL_CMD(
        text,
        NULL,
        "Text: display text x y message",
        command_text
    ),

    SHELL_CMD(
        hello,
        NULL,
        "Draw Hello World",
        command_hello
    ),

    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(
    display,
    &display_commands,
    "Browser display commands",
    NULL
);

static int command_adc_start(
    const struct shell *shell,
    size_t argc,
    char **argv
)
{
    uint32_t blocks = 100;

    if (argc == 2) {
        char *end = NULL;
        unsigned long value = strtoul(argv[1], &end, 10);

        if (end == argv[1] || *end != '\0' ||
            value == 0 || value > 1000000UL) {
            shell_error(shell, "usage: adc start [blocks]");
            return -EINVAL;
        }

        blocks = (uint32_t)value;
    }

    int result = adc_dma_start(blocks);

    if (result < 0) {
        shell_error(shell, "ADC start failed: %d", result);
        return result;
    }

    shell_print(shell, "ADC capture started: %u blocks", blocks);
    return 0;
}

static int command_adc_stop(
    const struct shell *shell,
    size_t argc,
    char **argv
)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    adc_dma_stop();
    shell_print(shell, "ADC stopped");
    return 0;
}

static int command_adc_status(
    const struct shell *shell,
    size_t argc,
    char **argv
)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    struct adc_stream_stats stats;
    adc_dma_get_stats(&stats);

    shell_print(shell, "running: %s", stats.running ? "yes" : "no");
    shell_print(shell, "rate: %u samples/s", ADC_SAMPLE_RATE);
    shell_print(shell, "block: %u samples", ADC_SAMPLES_PER_BLOCK);
    shell_print(shell, "produced: %u", stats.produced_blocks);
    shell_print(shell, "displayed: %u", stats.sent_blocks);
    shell_print(shell, "dropped: %u", stats.dropped_blocks);
    shell_print(shell, "DMA errors: %u", stats.dma_errors);
    shell_print(shell, "remaining: %u", stats.blocks_remaining);

    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    adc_commands,

    SHELL_CMD_ARG(
        start,
        NULL,
        "Start capture: adc start [blocks]",
        command_adc_start,
        1,
        1
    ),

    SHELL_CMD(
        stop,
        NULL,
        "Stop ADC capture",
        command_adc_stop
    ),

    SHELL_CMD(
        status,
        NULL,
        "Show ADC/DMA status",
        command_adc_status
    ),

    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(
    adc,
    &adc_commands,
    "ADC oscilloscope commands",
    NULL
);

static void wifi_auto_connect(void)
{
    struct net_if *iface;

    /*
     * WLAN-Treiber nach dem Systemstart kurz Zeit geben.
     */
    k_sleep(K_SECONDS(2));

    iface = net_if_get_first_wifi();

    if (iface == NULL) {
        printk("WiFi: no interface found\n");
        return;
    }

    int result = net_mgmt(
        NET_REQUEST_WIFI_CONNECT_STORED,
        iface,
        NULL,
        0
    );

    if (result < 0) {
        printk(
            "WiFi: stored connection failed: %d\n",
            result
        );
        return;
    }

    printk("WiFi: connection requested\n");
}

static int command_system_load(
    const struct shell *shell,
    size_t argc,
    char **argv
)
{
    ARG_UNUSED(shell);
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    printk("\nSystem load:\n");
    thread_analyzer_print(0);

    return 0;
}

static int command_info(
    const struct shell *shell,
    size_t argc,
    char **argv
)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(shell, "uka Zephyr WebScope");
    shell_print(shell, "firmware: %s", WEBSCOPE_VERSION);
    shell_print(shell, "built: %s %s", __DATE__, __TIME__);
    shell_print(shell, "Zephyr: %s", KERNEL_VERSION_STRING);
    shell_print(shell, "HTTP port: %d", HTTP_PORT);
    shell_print(shell, "stream: %s", WEBSCOPE_STREAM_MODE);
    shell_print(shell, "ADC input: GP26 / ADC0");
    shell_print(shell, "ADC rate: %u samples/s", ADC_SAMPLE_RATE);

    return 0;
}

SHELL_CMD_REGISTER(
    info,
    NULL,
    "Show firmware and system information",
    command_info
);

SHELL_STATIC_SUBCMD_SET_CREATE(
    system_commands,

    SHELL_CMD(
        load,
        NULL,
        "Show CPU and stack utilization",
        command_system_load
    ),

    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(
    system,
    &system_commands,
    "System information",
    NULL
);

int main(void)
{
    int result = adc_dma_init(scope_adc_block);

    printk("\n");
    printk("uka Zephyr WebScope ready\n");
    printk("---------------------\n");
    printk("Firmware version: %s\n", WEBSCOPE_VERSION);
    printk("Build: %s %s\n", __DATE__, __TIME__);
    printk("HTTP server port: %d\n", HTTP_PORT);
    printk("FFT stream path: /fft-stream\n");
    printk("Stream mode: %s\n", WEBSCOPE_STREAM_MODE);

    if (result < 0) {
        printk("ADC DMA init failed: %d\n", result);
    } else {
        printk("ADC input: GP26 / ADC0\n");
        printk("ADC rate: %u samples/s\n", ADC_SAMPLE_RATE);
    }

    wifi_auto_connect();

    return 0;
}
