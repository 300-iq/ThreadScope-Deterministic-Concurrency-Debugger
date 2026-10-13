import { parseJson } from '../shared/json.ts';
export async function api<T>(path:string,body?:unknown,signal?:AbortSignal):Promise<T> {
  const response=await fetch(`/api/${path}`,{method:body===undefined ? 'GET' : 'POST',headers:body===undefined ? {} : {'Content-Type':'application/json'},body:body===undefined ? undefined : JSON.stringify(body),signal});
  let data:unknown;
  try {data=parseJson(await response.text());} catch {throw new Error(`Local API returned invalid JSON (HTTP ${response.status}). Check the local server and frontend origin.`);}
  if(!response.ok) throw new Error(typeof data==='object' && data!==null && 'error' in data && typeof data.error==='string' ? data.error : `API error ${response.status}`);
  return data as T;
}
