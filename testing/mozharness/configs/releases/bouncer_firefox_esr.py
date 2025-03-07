# lint_ignore=E501
config = {
    "shipped-locales-url": "https://hg.mozilla.org/%(repo)s/raw-file/%(revision)s/browser/locales/shipped-locales",
    "bouncer_prefix": "https://download.mozilla.org/",
    "products": {
        "installer": {
            "product-name": "Firefox-%(version)s",
            "check_uptake": True,
            # convert to firefox-esr-latest when ESR52 stops
            "alias": "firefox-esr-next-latest",
            "ssl-only": True,
            "add-locales": True,
            "paths": {
                "linux": {
                    "path": "/firefox/releases/%(version)s/linux-i686/:lang/firefox-%(version)s.tar.bz2",
                    "bouncer-platform": "linux",
                },
                "linux64": {
                    "path": "/firefox/releases/%(version)s/linux-x86_64/:lang/firefox-%(version)s.tar.bz2",
                    "bouncer-platform": "linux64",
                },
                "macosx64": {
                    "path": "/firefox/releases/%(version)s/mac/:lang/Firefox%%20%(version)s.dmg",
                    "bouncer-platform": "osx",
                },
                "win32": {
                    "path": "/firefox/releases/%(version)s/win32/:lang/Firefox%%20Setup%%20%(version)s.exe",
                    "bouncer-platform": "win",
                },
                "win64": {
                    "path": "/firefox/releases/%(version)s/win64/:lang/Firefox%%20Setup%%20%(version)s.exe",
                    "bouncer-platform": "win64",
                },
            },
        },
        "installer-ssl": {
            "product-name": "Firefox-%(version)s-SSL",
            "check_uptake": True,
            # convert to firefox-esr-latest-ssl when ESR52 stops
            "alias": "firefox-esr-next-latest-ssl",
            "ssl-only": True,
            "add-locales": True,
            "paths": {
                "linux": {
                    "path": "/firefox/releases/%(version)s/linux-i686/:lang/firefox-%(version)s.tar.bz2",
                    "bouncer-platform": "linux",
                },
                "linux64": {
                    "path": "/firefox/releases/%(version)s/linux-x86_64/:lang/firefox-%(version)s.tar.bz2",
                    "bouncer-platform": "linux64",
                },
                "macosx64": {
                    "path": "/firefox/releases/%(version)s/mac/:lang/Firefox%%20%(version)s.dmg",
                    "bouncer-platform": "osx",
                },
                "win32": {
                    "path": "/firefox/releases/%(version)s/win32/:lang/Firefox%%20Setup%%20%(version)s.exe",
                    "bouncer-platform": "win",
                },
                "win64": {
                    "path": "/firefox/releases/%(version)s/win64/:lang/Firefox%%20Setup%%20%(version)s.exe",
                    "bouncer-platform": "win64",
                },
            },
        },
        "msi": {
            "product-name": "Firefox-%(version)s-msi-SSL",
            "check_uptake": True,
            "alias": "firefox-esr-msi-latest-ssl",
            "ssl-only": True,
            "add-locales": True,
            "paths": {
                "win32": {
                    "path": "/firefox/releases/%(version)s/win32/:lang/Firefox%%20Setup%%20%(version)s.msi",
                    "bouncer-platform": "win",
                },
                "win64": {
                    "path": "/firefox/releases/%(version)s/win64/:lang/Firefox%%20Setup%%20%(version)s.msi",
                    "bouncer-platform": "win64",
                },
            },
        },
        "complete-mar": {
            "product-name": "Firefox-%(version)s-Complete",
            "check_uptake": True,
            "ssl-only": False,
            "add-locales": True,
            "paths": {
                "linux": {
                    "path": "/firefox/releases/%(version)s/update/linux-i686/:lang/firefox-%(version)s.complete.mar",
                    "bouncer-platform": "linux",
                },
                "linux64": {
                    "path": "/firefox/releases/%(version)s/update/linux-x86_64/:lang/firefox-%(version)s.complete.mar",
                    "bouncer-platform": "linux64",
                },
                "macosx64": {
                    "path": "/firefox/releases/%(version)s/update/mac/:lang/firefox-%(version)s.complete.mar",
                    "bouncer-platform": "osx",
                },
                "win32": {
                    "path": "/firefox/releases/%(version)s/update/win32/:lang/firefox-%(version)s.complete.mar",
                    "bouncer-platform": "win",
                },
                "win64": {
                    "path": "/firefox/releases/%(version)s/update/win64/:lang/firefox-%(version)s.complete.mar",
                    "bouncer-platform": "win64",
                },
            },
        },
    },
    "partials": {
        "releases-dir": {
            "product-name": "Firefox-%(version)s-Partial-%(prev_version)s",
            "check_uptake": True,
            "ssl-only": False,
            "add-locales": True,
            "paths": {
                "linux": {
                    "path": "/firefox/releases/%(version)s/update/linux-i686/:lang/firefox-%(prev_version)s-%(version)s.partial.mar",
                    "bouncer-platform": "linux",
                },
                "linux64": {
                    "path": "/firefox/releases/%(version)s/update/linux-x86_64/:lang/firefox-%(prev_version)s-%(version)s.partial.mar",
                    "bouncer-platform": "linux64",
                },
                "macosx64": {
                    "path": "/firefox/releases/%(version)s/update/mac/:lang/firefox-%(prev_version)s-%(version)s.partial.mar",
                    "bouncer-platform": "osx",
                },
                "win32": {
                    "path": "/firefox/releases/%(version)s/update/win32/:lang/firefox-%(prev_version)s-%(version)s.partial.mar",
                    "bouncer-platform": "win",
                },
                "win64": {
                    "path": "/firefox/releases/%(version)s/update/win64/:lang/firefox-%(prev_version)s-%(version)s.partial.mar",
                    "bouncer-platform": "win64",
                },
            },
        },
    },
}
