import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter, once } from 'node:events';
import { PassThrough } from 'node:stream';
import { createRequire, syncBuiltinESMExports } from 'node:module';
import { invokeEngine } from '../server/engine.ts';
const childProcess=createRequire(import.meta.url)('node:child_process');
// Controlled lifecycle tests complement real-binary API tests. This fake does
// not produce execution results; it exercises kill/close/output/error handling.
class Child extends EventEmitter {
  stdout=new PassThrough();stderr=new PassThrough();signal:string|undefined;
  kill(signal:string) {this.signal=signal;this.emit('killed');return true;}
}
function setup(context:any) {
  const child=new Child();context.mock.method(childProcess,'spawn',(_path:string,_args:string[],options:any)=>{
    assert.equal(options.shell,false);return child;
  });syncBuiltinESMExports();context.after(()=>{context.mock.restoreAll();syncBuiltinESMExports();});return child;
}
test('watchdog kills and waits for child close before rejecting',async context=>{
  const child=setup(context);let settled=false;
  const promise=invokeEngine(['--help'],undefined,5);void promise.then(()=>{settled=true;},()=>{settled=true;});
  await once(child,'killed');assert.equal(child.signal,'SIGKILL');assert.equal(settled,false);
  child.emit('close',null,'SIGKILL');await assert.rejects(promise,/watchdog/);
});
test('abort and excessive output terminate children and cannot become verified results',async context=>{
  const child=setup(context);const abort=new AbortController();const promise=invokeEngine(['--help'],abort.signal);
  abort.abort();assert.equal(child.signal,'SIGKILL');child.emit('close',null,'SIGKILL');await assert.rejects(promise,/cancelled/);
  const another=invokeEngine(['--help']);child.stdout.write(Buffer.alloc(512*1024+1));
  assert.equal(child.signal,'SIGKILL');child.emit('close',null,'SIGKILL');await assert.rejects(another,/output exceeded/);
});
test('spawn failure is an unavailable engine, not a successful result',async context=>{
  const child=setup(context);const promise=invokeEngine(['--help']);child.emit('error',new Error('ENOENT'));
  await assert.rejects(promise,/Cannot launch/);child.emit('close',-2,null);
});
