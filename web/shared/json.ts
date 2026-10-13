// JSON.parse alone silently accepts duplicate object keys. Bound nesting and
// reject ambiguous keys before parsing imports, API bodies and engine files.
export function parseJson(input:string):unknown {
  const stack: ({keys:Set<string>;expectKey:boolean}|null)[]=[];
  let start=-1, escaped=false, keyOwner:{keys:Set<string>;expectKey:boolean}|null=null;
  for(let i=0;i<input.length;i++) {
    const char=input[i];
    if(start>=0) {
      if(escaped) {escaped=false;continue;}
      if(char==='\\') {escaped=true;continue;}
      if(char==='"') {
        if(keyOwner) {
          const key=JSON.parse(input.slice(start,i+1)) as string;
          if(keyOwner.keys.has(key)) throw new Error(`Duplicate JSON object key: ${key}`);
          keyOwner.keys.add(key);keyOwner.expectKey=false;
        }
        start=-1;keyOwner=null;
      }
      continue;
    }
    if(char==='"') {start=i;keyOwner=stack.at(-1)?.expectKey ? stack.at(-1)! : null;}
    else if(char==='{' || char==='[') {
      stack.push(char==='{' ? {keys:new Set(),expectKey:true} : null);
      if(stack.length>32) throw new Error('JSON nesting exceeds 32 containers.');
    } else if(char==='}' || char===']') stack.pop();
    else if(char===',' && stack.at(-1)) stack.at(-1)!.expectKey=true;
  }
  return JSON.parse(input);
}
