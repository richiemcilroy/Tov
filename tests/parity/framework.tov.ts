// The Tov port of framework.ts: the same app, with `Js` for `any` and `try` on calls that can
// throw. Diff them to see every edit.
import { createApp } from "parity-framework"

class Refused extends Error {
  constructor(message: string) {
    super(message)
    this.name = "Refused"
  }
}

function user(ctx: Js): Js {
  const p = ctx.params
  return `hello ${p.name}`
}

function readName(ctx: Js): Js {
  return ctx.params.name
}

function helper(ctx: Js): Js {
  const name = readName(ctx)
  return `helper ${name}`
}

function guarded(ctx: Js): Js {
  try {
    return `guarded ${try ctx.params.name}`
  } catch (e) {
    return `guarded caught ${e.name}: ${e.message}`
  }
}

async function main() {
  const app = try createApp()
  try app.route("/users/:name", user)
  try app.route("/helper/:name", helper)
  try app.route("/guarded/:name", guarded)
  try app.route("/async/:name", async (ctx: Js): Promise<Js> => {
    await Promise.resolve(0)
    return `async ${ctx.params.name}`
  })
  try app.route("/fail", (ctx: Js) => try ctx.fail("bang"))
  try app.route("/refused", (ctx: Js): Js => {
    throw new Refused("not today")
  })
  try app.route("/query", (ctx: Js) => `query ${ctx.query.q}`)
  try app.listen(Number(process.env.PORT ?? 3000))
}
