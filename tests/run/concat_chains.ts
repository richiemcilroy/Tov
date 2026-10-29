// `+` chains of strings, numbers and booleans: left to right, numbers added until a string comes.
const k = 7;
const x = 0.1 + 0.2;
const t = true;
console.log(1 + 2 + "x", "x" + 1 + 2, k + "v" + k, "a" + k + "b" + x + "c" + t);
console.log("" + -0 + "|" + NaN + "|" + 1e21 + "|" + 2 ** 53 + "|" + -1.5);
let i = 0;
const next = () => ++i;
console.log("n" + next() + next() + "-" + i);
const parts: string[] = [];
for (let j = 0; j < 3; j++) parts.push("item" + j + ":" + (j * 1.5) + (j % 2 === 0));
console.log(parts.join(" "));
const long = "z".repeat(70);
console.log(("<" + long + k + ">").length);
