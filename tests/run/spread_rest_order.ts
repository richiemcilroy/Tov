// An array literal with one spread copies the spread array before what follows is evaluated;
// a rest parameter gets the arguments in order.
const a = [1, 2];
const b = [...a, a.push(9), a.length];
console.log(b.join(","), a.join(","));
let k = 0;
const next = () => ++k;
const c = [next(), ...[10, 20], next()];
console.log(c.join(","));
const empty: number[] = [];
console.log([...empty].length, [0, ...empty, 1].join(","));
function count(...xs: string[]): string {
  return xs.length + ":" + xs.join("");
}
console.log(count(), count("a"), count("a", "b" + k, "c"));
const words = ["x", "y"];
console.log([...words, ...words].join(""), [..."hi", "!"].join(""));
