import { parseJson } from '../shared/json.ts';
import { createServer, type IncomingMessage, type ServerResponse } from 'node:http';
import { readFile } from 'node:fs/promises';
import { join, resolve, extname, sep } from 'node:path';
import { ApiError, EngineService, repository } from './engine.ts';
const allowedOrigins=new Set(['http://127.0.0.1:5173','http://localhost:5173','http://127.0.0.1:4318','http://localhost:4318']);
function send(response: ServerResponse,status: number,data: unknown) {
  response.writeHead(status,{'Content-Type':'application/json; charset=utf-8','Cache-Control':'no-store','X-Content-Type-Options':'nosniff'});
  response.end(JSON.stringify(data));
}
async function body(request: IncomingMessage): Promise<unknown> {
  if(request.headers['content-type']?.split(';')[0] !== 'application/json') throw new ApiError(415,'Use application/json.');
  if(Number(request.headers['content-length'])>8*1024*1024) throw new ApiError(413,'Request exceeds 8 MiB.');
  let bytes=0; const chunks: Buffer[]=[];
  for await(const chunk of request) { bytes+=chunk.length; if(bytes>8*1024*1024) throw new ApiError(413,'Request exceeds 8 MiB.'); chunks.push(chunk); }
  try { return parseJson(Buffer.concat(chunks).toString('utf8')); } catch(error) { throw new ApiError(400,`Invalid JSON request: ${error instanceof Error ? error.message : 'parse failure'}`); }
}
export function createApiServer() {
  const engine=new EngineService();
  const server=createServer(async(request,response)=>{
    const controller=new AbortController();
    request.once('aborted',()=>controller.abort());
    response.once('close',()=>{ if(!response.writableEnded) controller.abort(); });
    try {
      const host=request.headers.host;
      if(host !== `127.0.0.1:${request.socket.localPort}` && host !== `localhost:${request.socket.localPort}`) throw new ApiError(403,'Only localhost hosts are allowed.');
      if(request.headers.origin && !allowedOrigins.has(request.headers.origin)) throw new ApiError(403,'Only the local ThreadScope UI may use this API.');
      const path=new URL(request.url ?? '/',`http://${host}`).pathname;
      if(path==='/api/scenarios' && request.method==='GET') return send(response,200,{scenarios:await engine.scenarios(controller.signal)});
      if(path==='/api/explore' && request.method==='POST') return send(response,200,await engine.explore(await body(request),controller.signal));
      if(path==='/api/replay' && request.method==='POST') return send(response,200,await engine.replay(await body(request),controller.signal));
      if(path==='/api/branch' && request.method==='POST') return send(response,200,await engine.branch(await body(request),controller.signal));
      if(path==='/api/patch-verify' && request.method==='POST') return send(response,200,await engine.patchVerify(await body(request),controller.signal));
      if(path.startsWith('/api/')) throw new ApiError(404,'Unknown API route or method.');
      if(request.method !== 'GET') throw new ApiError(405,'Method not allowed.');
      const root=join(repository,'web','dist'); const file=resolve(root,`.${decodeURIComponent(path === '/' ? '/index.html' : path)}`);
      if(!file.startsWith(`${root}${sep}`)) throw new ApiError(403,'Invalid asset path.');
      let contents: Buffer; try { contents=await readFile(file); } catch { throw new ApiError(404,'Build the frontend with npm run build, or use npm run dev.'); }
      const types: Record<string,string>={'.html':'text/html','.js':'text/javascript','.css':'text/css','.svg':'image/svg+xml'};
      response.writeHead(200,{'Content-Type':types[extname(file)] ?? 'application/octet-stream','X-Content-Type-Options':'nosniff','Content-Security-Policy':"default-src 'self'; style-src 'self' 'unsafe-inline'; object-src 'none'; frame-ancestors 'none'"}); response.end(contents);
    } catch(error) {
      if(!response.destroyed) send(response,error instanceof ApiError ? error.status : 500,{error:error instanceof ApiError ? error.message : 'Unexpected local API failure. No verified result is available.'});
    }
  });
  server.requestTimeout=15_000;
  server.headersTimeout=10_000;
  return server;
}
