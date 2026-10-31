using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Text.RegularExpressions;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// HD menus: the game's menu textures upscaled on the player's PC from their own game files by
    /// 4x-NMKD-Siax through realesrgan-ncnn-vulkan, then finished at 2x by bb-ui-4k (the finest detail
    /// turned down and a plain resize mixed in): a conservative look that fixes the pixelated art
    /// without an oversharpened one. Built on request in two parts (Part), into engine-assets\ui-4k,
    /// the folder the start hands the engine (BB_UI_4K).
    /// </summary>
    public static class UiUpscale
    {
        public sealed record Step(string Text, double Fraction);

        private const string Release = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/";
        // 4x-NMKD-Siax (CX, 200k; WTFPL, by NMKD) as converted for ncnn in upscayl/custom-models.
        private const string Model = "4x_NMKD-Siax_200k";
        private const string Models = "https://raw.githubusercontent.com/upscayl/custom-models/4b6d2cfa59c7442af115dfc6e50fd8d7d40b96ef/models/";
        private const string ModelParamSha256 = "519bffb4912e295155d3cb5d6a29639d217ccca55f80962d8f677ef777a65b7a";
        private const string ModelBinSha256 = "b2abdffa30fb15be752ac681e0c0edf8b543745c33a989aadc9d7d3394dd3110";
        public const string Credit = "4x-NMKD-Siax (NMKD, WTFPL) through Real-ESRGAN ncnn Vulkan (Xintao Wang et al., MIT)";
        public const string Project = "https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan";
        /// <summary>The first line of a part's list: a part built another way is offered for a rebuild.</summary>
        // A part's recipe, the first line of its list: a build made by another is offered for a rebuild.
        // Menu art 2: with the HUD's cleaned art (hud-4k.bbui).
        private static string Recipe(Part part) => part == Part.MenuArt ? "siax-2x 2" : "siax-2x 1";

        private static (string Zip, long Size, string Sha256, string Exe) Package => OperatingSystem.IsWindows()
            ? ("realesrgan-ncnn-vulkan-20220424-windows.zip", 45474481, "abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d", "realesrgan-ncnn-vulkan.exe")
            : ("realesrgan-ncnn-vulkan-20220424-ubuntu.zip", 46931474, "e5aa6eb131234b87c0c51f82b89390f5e3e642b7b70f2b9bbe95b6a285a40c96", "realesrgan-ncnn-vulkan");

        private static string Assets => Path.Combine(Paths.DataDir, "engine-assets");
        public static string PackDir => Path.Combine(Assets, "ui-4k");
        private static string ToolDir => Path.Combine(Assets, "realesrgan");
        private static string WorkDir => Path.Combine(Assets, "ui-4k-work");
        private static string Marker => Path.Combine(PackDir, "complete");
        private static string Tool => Path.Combine(Paths.BinariesDir, Paths.Program("bb-ui-4k"));

        /// <summary>The two halves, built separately: the menu, HUD and icon art (a couple of minutes)
        /// and the 1056 item pictures, menu_knowledge_* (several minutes).</summary>
        public enum Part { MenuArt, ItemPictures }

        private static string Name(Part part) => part == Part.MenuArt ? "art" : "pictures";
        /// <summary>A built part's packs, one name a line after the recipe.</summary>
        private static string List(Part part) => Path.Combine(PackDir, Name(part) + ".list");
        private static Part Other(Part part) => part == Part.MenuArt ? Part.ItemPictures : Part.MenuArt;

        private static string[] Files(Part part) =>
            File.Exists(List(part)) ? File.ReadAllLines(List(part)).Skip(1).Where(name => name.Length > 0).ToArray() : Array.Empty<string>();

        /// <summary>A build of an earlier launcher (4x, one part, no lists).</summary>
        private static bool Legacy => File.Exists(Marker) && !File.Exists(List(Part.MenuArt)) && !File.Exists(List(Part.ItemPictures));

        /// <summary>The engine loads every pack in the folder once its marker is there; a part's packs
        /// only move in when the whole part is done, so what shows here is what the game loads.</summary>
        public static bool Ready => File.Exists(Marker);

        public static bool Built(Part part) => File.Exists(List(part)) || Legacy;

        public static bool UpToDate(Part part) => File.Exists(List(part)) && File.ReadLines(List(part)).FirstOrDefault() == Recipe(part);

        public static long PackBytes(Part part)
        {
            try
            {
                if (Legacy) return new DirectoryInfo(PackDir).EnumerateFiles().Sum(f => f.Length);
                return Files(part).Select(name => new FileInfo(Path.Combine(PackDir, name))).Where(f => f.Exists).Sum(f => f.Length);
            }
            catch (IOException)
            {
                return 0;
            }
        }

        /// <summary>Removes one part's packs and its unfinished build; with none left (or an earlier
        /// launcher's build), the whole folder.</summary>
        public static void Remove(Part part)
        {
            var work = Path.Combine(WorkDir, Name(part));
            if (Directory.Exists(work)) Directory.Delete(work, true);
            if (!Legacy)
            {
                DeletePacks(part);
                File.Delete(List(part));
                if (File.Exists(List(Other(part)))) return;
            }
            foreach (var dir in new[] { PackDir, WorkDir })
                if (Directory.Exists(dir)) Directory.Delete(dir, true);
        }

        /// <summary>The part's packs that the other part does not list too.</summary>
        private static void DeletePacks(Part part)
        {
            var other = Files(Other(part)).ToHashSet();
            foreach (var name in Files(part).Where(name => !other.Contains(name)))
                File.Delete(Path.Combine(PackDir, name));
        }

        public static async Task BuildAsync(string gameDir, Part part, IProgress<Step> progress, CancellationToken cancel)
        {
            var menu = Path.Combine(gameDir, "dvdroot_ps4", "menu");
            if (!Directory.Exists(menu))
                throw new InvalidOperationException(L.T("The game folder has no dvdroot_ps4\\menu: set the game folder first."));
            if (!File.Exists(Tool))
                throw new InvalidOperationException(L.T("This engine has no bb-ui-4k tool: update the engine first."));

            var upscaler = await EnsureUpscalerAsync(progress, cancel);
            // Each step keeps what it made, so a stopped build continues where it stopped.
            var work = Path.Combine(WorkDir, Name(part));
            var source = Path.Combine(work, "src");
            var upscaled = Path.Combine(work, "up");
            var todo = Path.Combine(work, "todo");
            var packed = Path.Combine(work, "pack");
            Directory.CreateDirectory(upscaled);

            progress.Report(new Step(L.T("Reading the menu textures from your game files…"), 0.05));
            await RunAsync(Tool, new[] { "export", menu, source, Name(part) }, null, cancel);

            var all = Directory.GetFiles(source, "*.png").Select(Path.GetFileName).ToList();
            var missing = all.Where(name => !File.Exists(Path.Combine(upscaled, name!))).ToList();
            if (Directory.Exists(todo)) Directory.Delete(todo, true);
            Directory.CreateDirectory(todo);
            foreach (var name in missing)
                File.Copy(Path.Combine(source, name!), Path.Combine(todo, name!));
            if (missing.Count > 0)
            {
                using var watch = new CancellationTokenSource();
                var counting = Task.Run(async () =>
                {
                    while (!watch.IsCancellationRequested)
                    {
                        var done = all.Count(name => File.Exists(Path.Combine(upscaled, name!)));
                        var text = part == Part.MenuArt
                            ? L.F($"Upscaling {done} of {all.Count} textures on the GPU…")
                            : L.F($"Upscaling {done} of {all.Count} item pictures on the GPU…");
                        progress.Report(new Step(text, 0.05 + 0.8 * done / all.Count));
                        try { await Task.Delay(1000, watch.Token); } catch (OperationCanceledException) { }
                    }
                });
                try
                {
                    var files = Math.Clamp(Environment.ProcessorCount / 2, 2, 8);
                    await RunAsync(upscaler, new[] { "-i", todo, "-o", upscaled, "-n", Model, "-f", "png", "-j", $"{files}:1:{files}" }, Path.GetDirectoryName(upscaler), cancel);
                }
                finally
                {
                    watch.Cancel();
                    await counting;
                }
            }
            var still = all.Count(name => !File.Exists(Path.Combine(upscaled, name!)));
            if (still > 0)
                throw new InvalidOperationException(L.F($"{still} textures were not upscaled. Your GPU driver may not support Vulkan compute; try again after a driver update."));

            var count = new Regex(@"^packed (\d+) of (\d+)$");
            await RunAsync(Tool, new[] { "pack", upscaled, packed, source }, null, cancel, line =>
            {
                if (count.Match(line) is { Success: true } m && int.TryParse(m.Groups[1].Value, out var n) && int.TryParse(m.Groups[2].Value, out var total) && total > 0)
                    progress.Report(new Step(L.F($"Packing {n} of {total} textures…"), 0.85 + 0.15 * n / total));
            });

            // The finished part replaces its earlier build (and an earlier launcher's build, all of it).
            if (Legacy) Directory.Delete(PackDir, true);
            Directory.CreateDirectory(PackDir);
            DeletePacks(part);
            var names = Directory.GetFiles(packed, "*.bbui").Select(file => Path.GetFileName(file)).ToList();
            foreach (var name in names)
                File.Move(Path.Combine(packed, name), Path.Combine(PackDir, name), overwrite: true);
            File.WriteAllLines(List(part), new[] { Recipe(part) }.Concat(names));
            if (!File.Exists(Marker)) File.WriteAllText(Marker, "\n");
            Directory.Delete(work, true);
            if (!Directory.EnumerateFileSystemEntries(WorkDir).Any()) Directory.Delete(WorkDir);
            progress.Report(new Step(L.T("HD menus are ready"), 1));
        }

        private static async Task<string> EnsureUpscalerAsync(IProgress<Step> progress, CancellationToken cancel)
        {
            var (zip, size, sha256, exe) = Package;
            var path = Path.Combine(ToolDir, exe);
            var models = Path.Combine(ToolDir, "models");
            if (!File.Exists(path))
            {
                Directory.CreateDirectory(ToolDir);
                var download = Path.Combine(ToolDir, zip);
                var report = new Progress<double>(p => progress.Report(new Step(L.F($"Downloading the upscaler {p:P0}…"), 0.04 * p)));
                await Updater.DownloadAsync(Release + zip, download, size, report, sha256);
                cancel.ThrowIfCancellationRequested();
                using (var archive = ZipFile.OpenRead(download))
                {
                    foreach (var entry in archive.Entries)
                    {
                        var name = entry.FullName.Replace('\\', '/');
                        if (name != exe && name != "vcomp140.dll") continue;
                        entry.ExtractToFile(Path.Combine(ToolDir, name), true);
                    }
                }
                File.Delete(download);
                if (!OperatingSystem.IsWindows())
                    File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            }
            if (!File.Exists(Path.Combine(models, Model + ".bin")) || !File.Exists(Path.Combine(models, Model + ".param")))
            {
                Directory.CreateDirectory(models);
                progress.Report(new Step(L.T("Downloading the upscaling model…"), 0.04));
                await Updater.DownloadAsync(Models + Model + ".param", Path.Combine(models, Model + ".param"), 0, null, ModelParamSha256);
                await Updater.DownloadAsync(Models + Model + ".bin", Path.Combine(models, Model + ".bin"), 0, null, ModelBinSha256);
                cancel.ThrowIfCancellationRequested();
            }
            return path;
        }

        private static async Task RunAsync(string program, string[] args, string? workingDir, CancellationToken cancel, Action<string>? line = null)
        {
            var info = new ProcessStartInfo(program)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                WorkingDirectory = workingDir ?? Path.GetDirectoryName(program)!,
            };
            foreach (var arg in args) info.ArgumentList.Add(arg);
            using var process = Process.Start(info) ?? throw new InvalidOperationException(L.F($"{Path.GetFileName(program)} did not start"));
            var errors = new List<string>();
            process.OutputDataReceived += (_, e) => { if (e.Data != null) line?.Invoke(e.Data); };
            process.ErrorDataReceived += (_, e) => { if (e.Data != null) lock (errors) { errors.Add(e.Data); if (errors.Count > 20) errors.RemoveAt(0); } };
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            using (cancel.Register(() => { try { process.Kill(true); } catch (InvalidOperationException) { } }))
                await process.WaitForExitAsync();
            cancel.ThrowIfCancellationRequested();
            if (process.ExitCode != 0)
            {
                string tail;
                lock (errors) tail = string.Join("\n", errors.Where(e => !e.Contains('%')).TakeLast(5));
                throw new InvalidOperationException(L.F($"{Path.GetFileName(program)} stopped (exit {process.ExitCode}). {tail}"));
            }
        }
    }
}
