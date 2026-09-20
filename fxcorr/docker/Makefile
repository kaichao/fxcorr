# fxcorr/fxcorr 单镜像构建（两段式 Dockerfile 在本目录，构建上下文 = 仓库根 ../..）
IMAGE_NAME:=fxcorr/fxcorr
VERSION:=2.9.1

build:
	docker buildx build \
		--network=host \
		--build-arg VERSION=$(VERSION) \
		--label build_date=$$(date -u +"%Y-%m-%dT%H:%M:%SZ") \
		-f Dockerfile -t $(IMAGE_NAME):latest -t $(IMAGE_NAME):$(VERSION) --load ../..

push:
	docker push $(IMAGE_NAME):latest
	docker push $(IMAGE_NAME):$(VERSION)

clean:
	docker rmi $(IMAGE_NAME):latest $(IMAGE_NAME):$(VERSION)

.PHONY: build push clean
