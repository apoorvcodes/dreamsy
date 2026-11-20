// Pages are prerendered at build time, before the deploy's runtime env exists, so production builds default to the live site.
const fallback = process.env.NODE_ENV === "production" ? "https://dreamsy.dev" : "http://127.0.0.1:3100";
export const SITE_URL = (process.env.SITE_URL ?? process.env.NEXT_PUBLIC_SITE_URL ?? fallback).replace(/\/$/, "");
export const SITE_NAME = "Dreamsy";
export const SITE_DESCRIPTION = "A lightweight, open-source game engine with an AI builder. Describe a 2D or 2.5D game and play it on iOS, Android or the web.";
