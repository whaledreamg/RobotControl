import { createApp } from 'vue'
import { createPinia } from 'pinia'
import { createRouter, createWebHistory } from 'vue-router'
import Vant from 'vant'
import 'vant/lib/index.css'

import App from './App.vue'
import Home from './views/Home.vue'
import AgvConsole from './views/AgvConsole.vue'

const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', component: Home },
    { path: '/agv', component: AgvConsole },
  ],
})

createApp(App)
  .use(createPinia())
  .use(router)
  .use(Vant)
  .mount('#app')
