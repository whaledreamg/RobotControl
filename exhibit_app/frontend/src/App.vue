<template>
  <router-view />
</template>

<script setup>
import { useDeviceStore } from './stores/device'
const deviceStore = useDeviceStore()
deviceStore.connect()
</script>
