import { createServer } from 'vite';
import { createApiServer } from './app.ts';
const api=createApiServer();
api.on('error',error=>{ console.error(error.message); process.exit(1); });
await new Promise<void>(resolve=>api.listen(4318,'127.0.0.1',resolve));
const vite=await createServer();
try { await vite.listen(); } catch(error) { api.close(); throw error; }
vite.printUrls(); console.log('Local engine API: http://127.0.0.1:4318');
for(const signal of ['SIGINT','SIGTERM'] as const) process.once(signal,async()=>{ await vite.close(); api.close(); });
