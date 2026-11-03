import express from "express";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { ensureBucket, BUCKET } from "./s3.js";
import { router } from "./routes/files.js";

const here = dirname(fileURLToPath(import.meta.url));
const app = express();
const port = Number(process.env.PORT) || 3000;

app.use(express.json({ limit: "1mb" }));
app.use(express.static(resolve(here, "../../public")));
app.use("/api/files", router);

app.use((err, req, res, next) => {
  if (res.headersSent) return next(err);
  const status = err.status || 500;
  if (status >= 500) console.error(err);
  res.status(status).json({ error: err.message });
});

await ensureBucket();
app.listen(port, () => {
  console.log(`node-sharp running at http://localhost:${port}, bucket ${BUCKET}`);
});
