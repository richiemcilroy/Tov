// `x ??= v` evaluates v only when x is null or undefined; `a || b` and `a && b` pass a value through.
let calls = 0;
function fallback(): number[] {
  calls++;
  return [0];
}
function maybe(i: number): number[] | undefined {
  return i % 2 ? [i, i + 1] : undefined;
}
let total = 0;
for (let i = 0; i < 6; i++) {
  let a = maybe(i);
  a ??= fallback();
  total += a.length;
}
console.log(total, calls);

function words(s: string): string[] {
  return s.match(/\w+/g) || [];
}
function firstOr(xs: number[] | undefined): number[] {
  return xs || [];
}
console.log(words("a bb ccc").join("|"), words("  ").length, firstOr([4, 5]).join(","), firstOr(undefined).length);
const both = maybe(1) && maybe(3);
console.log(both ? both.join(",") : "none", (maybe(2) && maybe(3)) ?? "none");
