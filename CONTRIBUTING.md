# Contributing Guidelines

Thank you for your interest in contributing to `electron-native-screenshare`! We welcome contributions from the community.

## How Can I Contribute?

### 1. Reporting Bugs
If you find a bug, please help us by opening an issue using the [Bug Report template](.github/ISSUE_TEMPLATE/bug_report.md). Please include as much detail as possible to help us reproduce and fix the bug.

### 2. Suggesting Enhancements
If you have an idea for a new feature or improvement, please let us know by opening an issue using the [Feature Request template](.github/ISSUE_TEMPLATE/feature_request.md). 

### 3. Pull Requests
1. Fork the repository.
2. Create a new branch for your feature or bugfix: `git checkout -b feature/your-feature-name` or `git checkout -b fix/your-bugfix-name`.
3. Make your changes.
4. Ensure the project builds successfully by running `npm run build`.
5. Run the tests to make sure everything works: `npm run test`.
6. Commit your changes: `git commit -m 'Add some feature'`.
7. Push to your branch: `git push origin feature/your-feature-name`.
8. Open a **Pull Request (PR)** against the `main` branch. Please fill out the provided PR template.

## Development Setup
This project uses C++ and `node-addon-api`. You will need standard build tools installed on your OS:
- **Windows**: Visual Studio with Desktop development with C++
- **macOS**: Xcode Command Line Tools
- **Linux**: GCC/G++ and make

1. Install dependencies: `npm install`
2. Build the addon: `npm run build`
3. Run tests: `npm run test`

## Coding Standards
* Ensure your code is clean, well-documented, and follows the existing style in the repository.
* Do not submit code that breaks existing functionality unless absolutely necessary and documented.

Thank you for contributing!
