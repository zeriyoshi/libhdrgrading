/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 *
 * The theme switch: the system's by default, the viewer's choice remembered
 * where storage works (the pages read the same without it).
 */
(function () {
    var root = document.documentElement, saved = null;

    try {
        saved = window.localStorage.getItem('hdrg-manual-theme');
    } catch (e) {
        saved = null;
    }

    if (saved === 'light' || saved === 'dark') {
        root.setAttribute('data-theme', saved);
    }

    document.addEventListener('DOMContentLoaded', function () {
        var buttons = document.querySelectorAll('button.theme'), i;

        for (i = 0; i < buttons.length; i++) {
            buttons[i].addEventListener('click', function () {
                var dark = root.getAttribute('data-theme') === 'dark' ||
                           (!root.hasAttribute('data-theme') &&
                            window.matchMedia('(prefers-color-scheme: dark)').matches),
                    next = dark ? 'light' : 'dark';

                root.setAttribute('data-theme', next);

                try {
                    window.localStorage.setItem('hdrg-manual-theme', next);
                } catch (e) {
                    /* the choice lasts for this page only */
                }
            });
        }
    });
})();
