# Contributing

Open elish driver, patcher, panel, and documentation changes against this repository. Include the exact change, the original and generated file hashes when relevant, and the checks you ran. If a change was tested on hardware, include the device variant and the observed result.

## Sending the SOID fix to Mu-Silicium

1. Fork [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) on GitHub, then clone your fork.
2. Set the upstream remote and branch from its default branch.

   ```sh
   git remote add upstream https://github.com/Project-Silicium/Mu-Silicium.git
   git fetch upstream
   git remote set-head upstream --auto
   git switch -c fix/chipinfo-output-width upstream/HEAD
   ```

3. Run `git apply --check` with [the SOID patch](uefi/0001-fix-chipinfo-output-width.patch), apply it, and inspect the diff. If the context has changed, adapt the two ChipInfo calls to the current source and recheck the diff. Keep the upstream file's copyright and license notices.
4. Commit the UEFI fix and push the branch to your fork.

   ```sh
   git add Silicon/Qualcomm/KonaPkg/Library/AcpiTableUpdateLib/AcpiTableUpdate.c
   git commit -m "Fix ChipInfo output widths in Kona ACPI updates"
   git push -u origin fix/chipinfo-output-width
   ```

5. Open a PR from that branch to the Mu-Silicium default branch. Describe the 32-bit writes into 16-bit ACPI fields and the observed `SOID = 0x164` result on elish. [Creating a PR from a fork](https://docs.github.com/en/pull-requests/how-tos/create-pull-requests/creating-a-pull-request-from-a-fork).
