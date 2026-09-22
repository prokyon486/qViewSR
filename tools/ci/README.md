# qViewSR CI migration

`ubuntu-24.04-workflows.patch` replaces upstream Qt 5 packaging and duplicate checks with the supported Ubuntu 24.04 viewer/worker build and tests. It is kept as a patch because the GitHub OAuth credential used for this change lacks the `workflow` scope. The application, tests, and local build are pushed normally; GitHub Actions migration has not been applied or verified remotely.

With a credential authorized to edit workflows, from the repository root:

```bash
git apply tools/ci/ubuntu-24.04-workflows.patch
git add .github/workflows
git commit -m "Use Ubuntu 24.04 qViewSR build and tests in CI"
git push
```

The equivalent local build and tests have passed via `tools/build_qviewsr.sh`. Upstream workflows remaining in `.github/workflows` are not supported checks for this prototype.
