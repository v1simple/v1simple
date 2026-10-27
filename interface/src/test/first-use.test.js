import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { JSDOM } from 'jsdom';
import { afterEach, describe, expect, it, vi } from 'vitest';

const guideHtml = readFileSync(resolve('static/first-use.html'), 'utf8');
const openGuides = [];

function openGuide(url = 'http://192.168.35.5/first-use.html', scripts = true) {
    const calls = { fetch: vi.fn(), xhr: vi.fn(), socket: vi.fn(), storage: vi.fn(), timer: vi.fn() };
    const dom = new JSDOM(guideHtml, {
        url,
        ...(scripts ? { runScripts: 'dangerously' } : {}),
        beforeParse(window) {
            window.fetch = calls.fetch;
            window.XMLHttpRequest = calls.xhr;
            window.WebSocket = calls.socket;
            window.Storage.prototype.setItem = calls.storage;
            window.Storage.prototype.removeItem = calls.storage;
            window.Storage.prototype.clear = calls.storage;
            window.setInterval = calls.timer;
            window.setTimeout = calls.timer;
        }
    });
    openGuides.push(dom);
    return { dom, document: dom.window.document, calls };
}

afterEach(() => {
    for (const dom of openGuides.splice(0)) dom.window.close();
});

describe('shared offline first-use guide', () => {
    it('keeps every setup, scene and step interaction local without network, storage or timers', () => {
        const { document, calls } = openGuide();
        for (const input of document.querySelectorAll('input[name="setup"]')) input.click();
        for (const button of document.querySelectorAll('[data-scene]')) button.click();
        document.querySelector('#mute-example').click();
        document.querySelector('#mute-example').click();
        for (const button of document.querySelectorAll('[data-go]')) button.click();

        for (const call of Object.values(calls)) expect(call).not.toHaveBeenCalled();
        expect(document.querySelector('[src], link[href], iframe, form, audio, video, meta[http-equiv]')).toBeNull();
        expect(document.body.textContent).toContain('This choice changes the guide only.');
        expect(document.body.textContent).toContain('It plays no sound and sends nothing to your detector.');
        expect(document.body.textContent).toContain('This guide does not keep maintenance alive.');
    });

    it('changes setup advice without claiming to save or connect the selected mode', () => {
        const { document } = openGuide();
        document.querySelector('input[value="proxy"]').click();
        expect(document.querySelector('#mode-advice').textContent).toContain('V1-Proxy by default');
        expect(document.querySelector('#mode-advice').textContent).toContain('OBD is disabled');
        document.querySelector('input[value="obd"]').click();
        expect(document.querySelector('#mode-advice').textContent).toContain('phone proxy is disabled');
        expect(document.querySelector('#mode-advice').textContent).toContain('Return to normal operation');
        document.querySelector('input[value="display"]').click();
        expect(document.querySelector('#mode-advice h3').textContent).toBe('Start with Display only');
    });

    it('shows retained and new-live meanings with text alternatives and a visible muted illustration', () => {
        const { document } = openGuide();
        document.querySelector('[data-go="1"]').click();
        expect(document.querySelector('[data-step="1"]').hidden).toBe(false);
        expect(document.activeElement.id).toBe('learn-title');
        expect(document.querySelector('#step-nav [data-go="1"]').getAttribute('aria-current')).toBe('step');
        document.querySelector('[data-scene="retained"]').click();
        expect(document.querySelector('#signal-state').textContent).toBe('Retained after clearing');
        expect(document.querySelector('#scene-explanation').textContent).toContain('does not mean the detector still receives');
        document.querySelector('[data-scene="new"]').click();
        expect(document.querySelector('#example').getAttribute('aria-label')).toContain('new live signal, Ka, from behind');
        expect(document.querySelector('#scene-explanation').textContent).toContain('takes priority over the earlier retained K alert');
        expect(document.querySelectorAll('[data-scene][aria-pressed="true"]').length).toBe(1);
        document.querySelector('#mute-example').click();
        expect(document.querySelector('#mute-example').getAttribute('aria-pressed')).toBe('true');
        expect(document.querySelector('#example-audio').textContent).toBe('Illustration: muted · still visible');
        expect(document.querySelector('#band').textContent).toBe('Ka');
        expect(document.querySelector('#example').hidden).toBe(false);
        document.querySelector('#mute-example').click();
        expect(document.querySelector('#example-audio').textContent).toBe('Illustration: unmuted');
    });

    it.each([
        'http://192.168.35.5/first-use.html',
        'https://example.test/project/first-use.html',
        'file:///checkout/interface/static/first-use.html'
    ])('works at %s without context-dependent configuration links or invented readiness', (url) => {
        const { document } = openGuide(url);
        document.querySelector('[data-go="2"]').click();
        expect(document.querySelector('[data-step="2"]').hidden).toBe(false);
        expect(document.querySelectorAll('a[href]').length).toBe(0);
        expect(document.querySelector('#check-title').textContent).toBe('Check the real device while parked');
        expect(document.querySelector('[data-step="2"]').textContent).toContain('This page cannot see your detector.');
        expect(document.querySelector('[data-step="2"]').textContent).toContain('Sent, not verified');
        expect(document.querySelector('[data-step="2"]').textContent).toContain('Keep your saved settings while checking the connection');
    });

    it('keeps all instructions readable when scripts are disabled', () => {
        const { document, calls } = openGuide(undefined, false);
        expect(Array.from(document.querySelectorAll('[data-step]')).every((step) => !step.hidden)).toBe(true);
        expect(document.querySelector('#step-nav').hidden).toBe(true);
        expect(document.querySelector('#mute-example').hidden).toBe(true);
        expect(document.querySelectorAll('[data-step]').length).toBe(3);
        for (const call of Object.values(calls)) expect(call).not.toHaveBeenCalled();
    });
});
