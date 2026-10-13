import { createApiServer } from './app.ts';
const server=createApiServer();
server.listen(4318,'127.0.0.1',()=>console.log('ThreadScope: http://127.0.0.1:4318 (built UI + local API)'));
server.on('error',error=>{ console.error(error.message); process.exitCode=1; });
for(const signal of ['SIGINT','SIGTERM'] as const) process.once(signal,()=>server.close());
