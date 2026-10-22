import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { VitePWA } from 'vite-plugin-pwa'

// 展厅手机/平板后端地址：通过 .env 配置；默认连笔记本后端
export default defineConfig({
  plugins: [
    vue(),
    VitePWA({
      registerType: 'autoUpdate',
      includeAssets: ['favicon.svg'],
      manifest: {
        name: 'RobotControl 展厅',
        short_name: 'RobotCtrl',
        theme_color: '#1677ff',
        icons: [{ src: '/icon-192.png', sizes: '192x192', type: 'image/png' }],
      },
      workbox: {
        // 后端 REST/WS 永不缓存，H5 静态资源长缓存
        navigateFallbackDenylist: [/^\/api/, /^\/ws/],
      },
    }),
  ],
  server: {
    host: '0.0.0.0',
    port: 5173,
    proxy: {
      '/api': 'http://localhost:8000',
      '/ws': { target: 'ws://localhost:8000', ws: true },
    },
  },
})
