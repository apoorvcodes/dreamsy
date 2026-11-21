# Dreamsy site + editor on Fly.io: Next.js (Node) with the harness, the C++ engine and Chromium for playtest screenshots.
# Run deploy/fly/stage.sh first (engine binary, web player, seed games), then `fly deploy` from the repo root.
FROM mcr.microsoft.com/playwright:v1.65.1-noble
WORKDIR /app
ENV NEXT_TELEMETRY_DISABLED=1 \
    SITE_URL=https://dreamsy.dev
RUN npm i -g pnpm@10.4.1

COPY . .
RUN printf 'packages:\n  - harness\n  - format\n  - services/api\n  - apps/site\n' > pnpm-workspace.yaml \
 && pnpm install --no-frozen-lockfile --filter @dreamsy/site... --filter @dreamsy/harness... \
 && mkdir -p engine/build/release engine/build/web \
 && cp deploy/fly/artifacts/dreamsy-tool engine/build/release/dreamsy-tool \
 && chmod +x engine/build/release/dreamsy-tool \
 && cp -R deploy/fly/artifacts/web/. engine/build/web/ \
 && engine/build/release/dreamsy-tool version

RUN node apps/site/scripts/prepare-player.mjs engine/build/web \
 && cd apps/site && pnpm build

ENV NODE_ENV=production \
    PORT=8080 \
    DREAMSY_GAMES_DIR=/data/games \
    DREAMSY_HOME=/data/dreamsy
EXPOSE 8080
CMD ["sh", "deploy/fly/start.sh"]
