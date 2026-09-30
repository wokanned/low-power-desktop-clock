Add-Type -AssemblyName System.Runtime.WindowsRuntime
$asb = [System.Reflection.Assembly]::LoadWithPartialName('System.Runtime.WindowsRuntime')

[Windows.Media.Ocr.OcrEngine, Windows.Media.Ocr, ContentType=WindowsRuntime] | Out-Null
[Windows.Graphics.Imaging.BitmapDecoder, Windows.Graphics.Imaging, ContentType=WindowsRuntime] | Out-Null
[Windows.Storage.StorageFile, Windows.Storage, ContentType=WindowsRuntime] | Out-Null

function Get-TextFromImage {
    param([string]$ImagePath)
    $fileTask = [Windows.Storage.StorageFile]::GetFileFromPathAsync($ImagePath)
    $file = [System.WindowsRuntimeSystemExtensions]::GetAwaiter($fileTask).GetResult()
    
    $streamTask = $file.OpenAsync([Windows.Storage.FileAccessMode]::Read)
    $stream = [System.WindowsRuntimeSystemExtensions]::GetAwaiter($streamTask).GetResult()
    
    $decoderTask = [Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)
    $decoder = [System.WindowsRuntimeSystemExtensions]::GetAwaiter($decoderTask).GetResult()
    
    $bitmapTask = $decoder.GetSoftwareBitmapAsync()
    $bitmap = [System.WindowsRuntimeSystemExtensions]::GetAwaiter($bitmapTask).GetResult()
    
    $engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
    if ($null -eq $engine) { throw 'OCR Engine unavailable' }
    
    $resultTask = $engine.RecognizeAsync($bitmap)
    $result = [System.WindowsRuntimeSystemExtensions]::GetAwaiter($resultTask).GetResult()
    
    return $result.Text
}

try {
    Get-TextFromImage -ImagePath 'D:\codeOfVSCODE\desk clock\max17043_page10.png'
} catch {
    Write-Error $_.Exception.Message
    Write-Error $_.ScriptStackTrace
}
