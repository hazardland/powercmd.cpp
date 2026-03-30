<script setup>
import { features } from './data/features'

const heroDemo = features[0]
const heroVideo = '/videos/header.mp4'
const heroRatio = '8 / 11'
const heroTopTrim = '5.8%'
const heroChromeHeight = '42px'
</script>

<template>
  <div class="site-shell">
    <div class="atmosphere">
      <div class="halo halo-blue"></div>
      <div class="halo halo-gold"></div>
      <div class="shape-ribbon"></div>
      <div class="grid-fade"></div>
    </div>

    <header class="hero">
      <nav class="topbar">
        <div class="brand">ZCMD.DEV</div>
        <div class="topbar-links">
          <a href="#features">Features</a>
          <a href="#install">Install</a>
          <a href="https://github.com/hazardland/zcmd" target="_blank" rel="noreferrer">GitHub</a>
        </div>
      </nav>

      <section class="hero-grid">
        <div class="hero-copy">
          <p class="eyebrow">Windows-first shell</p>
          <h1>Zcmd is a visually refined shell for Windows developers: a fast, single-file, zero-config C++ alternative to cmd.exe.</h1>
          <p class="lede">
            Built-in tools, inline media, and a native visual identity make the command line feel faster, sharper, and far more enjoyable to use.
          </p>

          <div class="signal-row">
            <span>single executable</span>
            <span>zero config</span>
            <span>windows-native</span>
            <span>built in C++</span>
          </div>

          <div class="hero-actions">
            <a class="button button-primary" href="https://github.com/hazardland/zcmd/releases/latest" target="_blank" rel="noreferrer">
              <span>Download Latest</span>
              <small>GitHub Release</small>
            </a>
            <a class="button button-secondary" href="https://github.com/hazardland/zcmd" target="_blank" rel="noreferrer">
              <svg class="link-icon" viewBox="0 0 24 24" aria-hidden="true">
                <path d="M12 .5C5.65.5.5 5.65.5 12c0 5.08 3.29 9.39 7.86 10.91.58.11.79-.25.79-.56v-1.97c-3.2.7-3.88-1.35-3.88-1.35-.52-1.33-1.28-1.68-1.28-1.68-1.04-.72.08-.71.08-.71 1.15.08 1.75 1.18 1.75 1.18 1.03 1.75 2.69 1.25 3.34.95.1-.74.4-1.25.73-1.54-2.55-.29-5.24-1.28-5.24-5.68 0-1.25.45-2.27 1.18-3.07-.12-.29-.51-1.46.11-3.02 0 0 .97-.31 3.17 1.17.92-.26 1.9-.39 2.88-.39s1.96.13 2.88.39c2.2-1.48 3.17-1.17 3.17-1.17.62 1.56.23 2.73.12 3.02.73.8 1.18 1.82 1.18 3.07 0 4.41-2.69 5.38-5.26 5.66.41.35.77 1.04.77 2.1v3.11c0 .31.21.68.8.56A11.5 11.5 0 0 0 23.5 12C23.5 5.65 18.35.5 12 .5Z" />
              </svg>
              <span>View Repo</span>
              <small>GitHub</small>
            </a>
          </div>

          <div class="install-card" id="install">
            <div class="install-row">
              <span class="install-label">Direct</span>
              <code>Download zcmd.exe from the latest GitHub release</code>
            </div>
            <div class="install-row">
              <span class="install-label">winget</span>
              <code>coming soon</code>
            </div>
          </div>
        </div>

        <div class="hero-terminal">
          <div
            class="video-frame hero-video-shell"
            :style="{
              aspectRatio: heroRatio,
              '--video-top-trim': heroTopTrim,
              '--video-chrome-height': heroChromeHeight,
            }"
          >
            <div class="video-chrome">
              <div class="video-chrome-title">zcmd.exe</div>
              <div class="window-controls video-controls">
                <span aria-hidden="true"></span>
                <span aria-hidden="true"></span>
                <span aria-hidden="true"></span>
              </div>
            </div>
            <video
              class="feature-demo"
              :src="heroVideo"
              autoplay
              muted
              loop
              playsinline
              preload="metadata"
            ></video>
          </div>
          <div class="terminal-window" v-if="false">
            <div class="terminal-chrome">
              <div class="window-title">zcmd.exe</div>
              <div class="window-controls">
                <span aria-hidden="true">─</span>
                <span aria-hidden="true">▢</span>
                <span aria-hidden="true">×</span>
              </div>
            </div>
            <div class="terminal-body">
              <p><span class="muted">[19:14:25.83]</span><span class="prompt">zcmd&gt;</span> play heliosphan.mp3</p>
              <p><span class="visualizer">[▁▃▆█▇▄▂▁▅▇█▆]</span> <span class="meta">01:24/04:57 [100%]</span> <span class="state">▶</span></p>
              <p><span class="muted">[19:14:29.02]</span><span class="prompt">zcmd&gt;</span> top</p>
              <p><span class="accent">cpu</span> 14% <span class="accent">mem</span> 824 MB <span class="accent">shell</span> native</p>
              <p><span class="muted">[19:14:33.17]</span><span class="prompt">zcmd&gt;</span> ls</p>
              <p class="terminal-footnote">single executable • zero config • built-in tools • media • visualizer</p>
            </div>
          </div>
        </div>
      </section>
    </header>

    <main>
      <section class="about">
        <div class="about-panel">
          <p class="eyebrow">Why it exists</p>
          <h2>Built because Windows deserved a shell with speed and style.</h2>
          <p>
            PowerShell felt slow, cmd.exe felt too limited, and the best shell quality-of-life still felt trapped on Linux.
            Zcmd brings that experience into a native Windows-first shell without turning setup into a project.
          </p>
        </div>
      </section>

      <section class="feature-stream" id="features">
        <article
          v-for="(feature, index) in features"
          :key="feature.id"
          class="feature-section"
          :class="{ reverse: index % 2 === 1 }"
        >
          <div class="feature-copy">
            <p class="feature-kicker">Feature {{ index + 1 }}</p>
            <h2>{{ feature.title }}</h2>
            <p>{{ feature.body }}</p>
          </div>

          <div class="feature-video">
            <div
              class="video-frame"
              :style="{
                aspectRatio: feature.ratio || '16 / 9',
                '--video-top-trim': feature.topTrim || '0%',
              }"
            >
              <div class="video-chrome">
                <div class="video-chrome-title">{{ feature.title }}</div>
                <div class="window-controls video-controls">
                  <span aria-hidden="true">─</span>
                  <span aria-hidden="true">▢</span>
                  <span aria-hidden="true">×</span>
                </div>
              </div>
              <video
                v-if="feature.video"
                class="feature-demo"
                :src="feature.video"
                autoplay
                muted
                loop
                playsinline
                preload="metadata"
              ></video>
              <div v-else class="video-placeholder">
                <span>Drop demo video here</span>
                <code>{{ feature.id }}.mp4</code>
              </div>
            </div>
          </div>
        </article>
      </section>

      <section class="more-tools">
        <p class="eyebrow">More built-ins</p>
        <h2>More Built-Ins Included</h2>
        <p>
          Zcmd also ships with extras like aliases, calculator, clock, stopwatch, JSON formatting,
          clipboard helpers, notes, IP tools, and more.
        </p>
      </section>
    </main>

    <footer class="site-footer">
      <p>Zcmd is a custom Windows shell for people who want the terminal to feel fast, visual, and fun.</p>
      <div class="footer-links">
        <a href="https://github.com/hazardland/zcmd" target="_blank" rel="noreferrer">
          <svg class="link-icon" viewBox="0 0 24 24" aria-hidden="true">
            <path d="M12 .5C5.65.5.5 5.65.5 12c0 5.08 3.29 9.39 7.86 10.91.58.11.79-.25.79-.56v-1.97c-3.2.7-3.88-1.35-3.88-1.35-.52-1.33-1.28-1.68-1.28-1.68-1.04-.72.08-.71.08-.71 1.15.08 1.75 1.18 1.75 1.18 1.03 1.75 2.69 1.25 3.34.95.1-.74.4-1.25.73-1.54-2.55-.29-5.24-1.28-5.24-5.68 0-1.25.45-2.27 1.18-3.07-.12-.29-.51-1.46.11-3.02 0 0 .97-.31 3.17 1.17.92-.26 1.9-.39 2.88-.39s1.96.13 2.88.39c2.2-1.48 3.17-1.17 3.17-1.17.62 1.56.23 2.73.12 3.02.73.8 1.18 1.82 1.18 3.07 0 4.41-2.69 5.38-5.26 5.66.41.35.77 1.04.77 2.1v3.11c0 .31.21.68.8.56A11.5 11.5 0 0 0 23.5 12C23.5 5.65 18.35.5 12 .5Z" />
          </svg>
          <span>GitHub</span>
        </a>
        <a href="https://github.com/hazardland/zcmd/releases/latest" target="_blank" rel="noreferrer">Latest Release on GitHub</a>
      </div>
    </footer>
  </div>
</template>
