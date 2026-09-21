1. C++ 빌드
```
cmake -S code -B code/build
cmake --build code/build --config Debug

code/build/backend/tests/Debug/engine_tests.exe
code/build/backend/cli/Debug/engine_cli.exe
code/build/backend/server/Debug/engine_server.exe
```

<br/>

2. UI 실행
```
cd code/frontend
npm install
npm run tauri dev
```

<br/>

3. Git

`main`이 현재 C++ 버전(실제 개발 브랜치)이고, `legacy`는 과거 Rust 버전을 보존만 해둔 아카이브 브랜치입니다(더 이상 개발 안 함).

```
git checkout main

git add -A
git commit -m "메시지"
git push origin 브랜치이름

git checkout -b 브랜치이름
git add -A
git commit -m "메시지"
git push -u origin 브랜치이름
```
