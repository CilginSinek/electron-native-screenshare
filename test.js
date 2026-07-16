const { startCapture, stopCapture } = require('./lib/index.js');

console.log('Test başlatılıyor... Ses yakalaması test ediliyor.');
console.log('Uygulama 10 saniye boyunca dinleyecek. Lütfen arka planda müzik/video açıp kapatmayı deneyin.');

try {
    const success = startCapture(process.pid, false, (data, meta) => {
        // Gelen veri Float32Array dizisine çevriliyor (çünkü kod isFloat: true döndürüyor ve 32bit)
        const floatArray = new Float32Array(data.buffer, data.byteOffset, data.length / 4);

        // Sinyalin gücü / yüksekliği (Volume/RMS) hesaplanıyor
        let sumSquares = 0;
        for (let i = 0; i < floatArray.length; i++) {
            sumSquares += floatArray[i] * floatArray[i];
        }
        const rms = Math.sqrt(sumSquares / floatArray.length);

        if (rms > 0.0001) {
            console.log(`[🔊 SES VAR!] Güç seviyesi (RMS): ${(rms).toFixed(4)} - ${(data.length / 1024).toFixed(1)} KB veri alındı.`);
        } else {
            // Sessizlik durumunda çok sık log basmamak için bir şey yazmayabiliriz,
            // ya da sessizlik olduğunu ara sıra belirtebiliriz.
            // console.log(`[🔇 SESSİZLİK] Sıfırlardan oluşan veri geldi: ${data.length} bytes`);
        }
    });

    if (success) {
        console.log('[BİLGİ] Ses yakalama aktif edildi. Bağlantı başarılı!');

        // 10 saniye sonra testi kapat
        setTimeout(() => {
            console.log('[BİLGİ] Test tamamlandı, çıkılıyor.');
            stopCapture();
            process.exit(0);
        }, 10000);
    } else {
        console.error('[HATA] startCapture false döndürdü.');
        process.exit(1);
    }
} catch (err) {
    console.error('[KRİTİK HATA] Exception fırlatıldı:', err);
    process.exit(1);
}
