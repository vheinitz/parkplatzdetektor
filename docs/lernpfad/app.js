// Energiebudget-Rechner für den Sensorknoten (Kapitel 4).
// Laufzeit = nutzbare Kapazität / mittlerer Strom. Alle Eingaben in den
// Einheiten, die neben dem Feld stehen; intern wird in mAh je Tag gerechnet.
(function () {
  var ids = ['cap', 'sleep', 'int', 'mdur', 'mcur', 'ev', 'hb', 'rep', 'tdur', 'tcur'];
  var el = {};
  ids.forEach(function (k) { el[k] = document.getElementById('c-' + k); });
  var out = {
    day: document.getElementById('o-day'),
    split: document.getElementById('o-split'),
    avg: document.getElementById('o-avg'),
    life: document.getElementById('o-life'),
    life2: document.getElementById('o-life2')
  };
  if (!el.cap || !out.day) return;

  // Drei Entwürfe. "heute" ist die Firmware im Repo: WLAN-Accesspoint und
  // Captive Portal laufen dauernd, der Chip schläft nie -> ~90 mA Ruhestrom.
  var presets = {
    heute:     { cap: 2000, sleep: 90000, int: 0.05, mdur: 0,  mcur: 0,  ev: 10, hb: 15, rep: 3, tdur: 60, tcur: 120 },
    ziel:      { cap: 2000, sleep: 10,    int: 2,    mdur: 20, mcur: 20, ev: 10, hb: 15, rep: 3, tdur: 60, tcur: 120 },
    industrie: { cap: 8000, sleep: 15,    int: 2,    mdur: 10, mcur: 15, ev: 10, hb: 60, rep: 2, tdur: 60, tcur: 120 }
  };

  function num(k) {
    var v = parseFloat(el[k].value);
    return isNaN(v) ? 0 : v;
  }

  function fmt(x, digits) {
    return x.toLocaleString('de-DE', { maximumFractionDigits: digits, minimumFractionDigits: 0 });
  }

  function calc() {
    var cap = num('cap');
    var sleepMah = num('sleep') / 1000 * 24;                       // µA -> mA, mal 24 h
    var wakes = num('int') > 0 ? 86400 / num('int') : 0;
    var measMah = wakes * num('mcur') * num('mdur') / 3600000;     // mA * ms -> mAh
    var events = num('ev') + (num('hb') > 0 ? 1440 / num('hb') : 0);
    var txMah = events * num('rep') * num('tcur') * num('tdur') / 3600000;
    var day = sleepMah + measMah + txMah;
    var avgMa = day / 24;
    var days = day > 0 ? cap / day : Infinity;

    out.day.textContent = fmt(day, day < 10 ? 2 : 0) + ' mAh';
    out.split.textContent = 'Schlaf ' + fmt(sleepMah, 2) + ' · Messen ' + fmt(measMah, 2) + ' · Funk ' + fmt(txMah, 2);
    out.avg.textContent = avgMa >= 1 ? fmt(avgMa, 1) + ' mA' : fmt(avgMa * 1000, 0) + ' µA';

    if (!isFinite(days)) {
      out.life.textContent = '∞';
      out.life2.textContent = '';
    } else if (days < 2) {
      out.life.textContent = fmt(days * 24, 0) + ' h';
      out.life2.textContent = 'die Zelle ist morgen leer';
    } else if (days < 400) {
      out.life.textContent = fmt(days, 0) + ' Tage';
      out.life2.textContent = '≈ ' + fmt(days / 30.4, 1) + ' Monate';
    } else {
      out.life.textContent = fmt(days / 365, 1) + ' Jahre';
      out.life2.textContent = fmt(days, 0) + ' Tage, Selbstentladung nicht gerechnet';
    }
  }

  function apply(name) {
    var p = presets[name];
    if (!p) return;
    ids.forEach(function (k) { el[k].value = p[k]; });
    document.querySelectorAll('#calc .presets button').forEach(function (b) {
      b.setAttribute('aria-pressed', b.dataset.preset === name ? 'true' : 'false');
    });
    calc();
  }

  document.querySelectorAll('#calc .presets button').forEach(function (b) {
    b.addEventListener('click', function () { apply(b.dataset.preset); });
  });
  ids.forEach(function (k) {
    el[k].addEventListener('input', function () {
      // Handeingabe: kein Preset mehr aktiv
      document.querySelectorAll('#calc .presets button').forEach(function (b) {
        b.setAttribute('aria-pressed', 'false');
      });
      calc();
    });
  });

  apply('heute');
})();
