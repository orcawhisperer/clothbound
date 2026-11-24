import { defineConfig } from "cf/config";

export default defineConfig({
  worker: {
    name: "clothbound-site",
    compatibilityDate: "2026-11-19",
    domains: ["clothbound.app", "www.clothbound.app"],
    assets: {
      htmlHandling: "auto-trailing-slash",
      notFoundHandling: "none",
    },
  },
});
