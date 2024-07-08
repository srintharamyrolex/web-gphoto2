export type { Config, SupportedOps } from '../build/libapi.mjs';
export declare function rethrowIfCritical(err: any): void;
export type CancellationToken = {
    isCancelled: boolean;
};
export declare class Camera {
    #private;
    destroyCamera(): void;
    static showPicker(): Promise<void>;
    static listAvailableCameras(): Promise<any>;
    connect(): Promise<void>;
    getConfig(): Promise<({
        name: string;
        info: string;
        label: string;
        readonly: boolean;
    } & {
        type: "window";
        children: Record<string, import("../build/libapi.mjs").Config>;
    } & {
        type: "window";
    }) | undefined>;
    getSupportedOps(): Promise<import("../build/libapi.mjs").SupportedOps>;
    setConfigValue(name: string, value: string | number | boolean): Promise<void>;
    capturePreviewAsBlob(): Promise<Blob | undefined>;
    captureImageAsFile(): Promise<File | undefined>;
    consumeEvents(): Promise<boolean | undefined>;
    static createCancellationToken(): CancellationToken;
    static cancelAll(): void;
}
