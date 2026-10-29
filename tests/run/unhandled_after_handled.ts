// A rejection nobody handles is still reported when many handled ones came before it.
async function fails(i: number): Promise<number> { if (i % 2) throw new Error("e" + i); return i }
async function main() {
  let n = 0
  for (let i = 0; i < 1000; i++) n += await fails(i).catch(() => -1)
  const lost = fails(7)
  for (let i = 0; i < 1000; i++) n += await fails(i).catch(() => -1)
  console.log(n)
}
main()
