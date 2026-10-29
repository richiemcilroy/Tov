// Looping over a record's keys reads each value from the entry it's at, also when the body
// builds arrays (no lookups, no array of keys); changes to the record's values are seen.
const o: Record<string, number[]> = { a: [1, 2], b: [], c: [3] };
let n = 0;
for (const k of Object.keys(o)) n += (o[k] ?? []).length + [k.length, 1].length;
console.log(n);
for (const k of Object.keys(o)) o[k] = [...(o[k] ?? []), 9];
console.log(JSON.stringify(o));
const counts: Record<string, number> = { x: 1, y: 2 };
for (const k of Object.keys(counts)) counts[k] = (counts[k] ?? 0) * 10 + [k].length;
console.log(JSON.stringify(counts));
