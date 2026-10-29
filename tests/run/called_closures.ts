// Local arrows only ever called (their environment on the stack): they see their captures as
// they are when called, loop variables included, and the ones that write, return or capture
// counted values stay ordinary closures.
let total = 0;
for (let i = 0; i < 5; i++) {
  const plus = (y: number) => y + i;
  total += plus(10);
}
console.log(total);

let c = 1;
const read = () => c * 10;
const before = read();
c = 7;
console.log(before, read(), c + read());

let n = 0;
const bump = () => { n += 1; return n; };
console.log(n, bump(), n + bump());

const label = "x".repeat(3);
const show = (k: number) => label + k;
console.log(show(1), show(2));

let flag = true;
const pick = (a: number, b: number) => (flag ? a : b);
const first = pick(1, 2);
flag = false;
console.log(first, pick(1, 2));

const later = () => z * 2;
let z = 4;
console.log(later());

const adders: Array<() => number> = [];
for (let j = 0; j < 3; j++) {
  const get = () => j;
  adders.push(get);
}
console.log(adders.map((f) => f()).join(","));
