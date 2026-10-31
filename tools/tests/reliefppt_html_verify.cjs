#!/usr/bin/env node
'use strict';
// Optional browser acceptance for a guest-produced offline HTML export.
// Run with Playwright on NODE_PATH and its Chromium runtime installed.
const assert = require('node:assert/strict');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const { chromium } = require('playwright');

async function main() {
    const [file, screenshot] = process.argv.slice(2);
    assert(file && screenshot, 'usage: reliefppt_html_verify.cjs export.html screenshot.png');
    const browser = await chromium.launch({
        headless: true,
        executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH || undefined
    });
    try {
        const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
        const errors = [], external = [];
        page.on('pageerror', error => errors.push(String(error)));
        page.on('request', request => {
            if (!/^(file:|data:)/.test(request.url())) external.push(request.url());
        });
        await page.goto(pathToFileURL(path.resolve(file)).href, { waitUntil: 'load' });
        const slides = page.locator('main img');
        assert.equal(await slides.count(), 4);
        assert(await slides.evaluateAll(items => items.every(item =>
            item.complete && item.naturalWidth === 960 && item.naturalHeight === 540 &&
            item.src.startsWith('data:image/png;base64,'))));
        const count = page.locator('#count');
        async function expect(number) {
            await page.waitForFunction(value => document.getElementById('count').textContent === `${value} / 4`, number);
            assert.equal(await count.textContent(), `${number} / 4`);
            assert.equal(await page.locator('main img.active').count(), 1);
            assert(await slides.nth(number - 1).isVisible());
        }
        await expect(1);
        for (const [key, number] of [['ArrowRight', 2], ['Home', 1], ['End', 4],
            ['PageUp', 3], ['Space', 4], ['ArrowLeft', 3]]) {
            await page.keyboard.press(key);
            await expect(number);
        }
        await page.locator('#next').click(); await expect(4);
        await page.locator('#prev').click(); await expect(3);
        await page.keyboard.press('b');
        assert.equal(await page.locator('main').evaluate(item => getComputedStyle(item).visibility), 'hidden');
        await page.keyboard.press('b');
        assert.equal(await page.locator('main').evaluate(item => getComputedStyle(item).visibility), 'visible');
        await page.locator('#full').click();
        await page.waitForFunction(() => document.fullscreenElement !== null);
        await page.locator('#full').click();
        await page.waitForFunction(() => document.fullscreenElement === null);
        await page.keyboard.press('Home'); await expect(1);
        await page.screenshot({ path: path.resolve(screenshot) });
        assert.deepEqual(errors, []);
        assert.deepEqual(external, []);
        console.log('ReliefPPT offline HTML browser tests passed: embedded images, navigation, black screen, fullscreen, no network');
    } finally {
        await browser.close();
    }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
